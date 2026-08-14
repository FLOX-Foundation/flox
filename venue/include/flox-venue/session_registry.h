/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Exec-report delivery: account -> session routing with per-session bounded
 * outbound queues.
 *
 * The per-frame Responder in the gateways answers only the connection that
 * carried the command; an asynchronous event (a maker fill from a foreign
 * aggressor, a stop trigger, a GTD expiry, a liquidation cancel, a FillHeld)
 * has no request context and previously had no path to its owner's session.
 * SessionRegistry closes that hole:
 *
 *   engine sink -> registry.route(event) -> AccountStream (per account)
 *     -> seq stamp + event log + encode -> SessionWriter (per connection)
 *       -> bounded queue -> writer thread -> socket
 *
 * Threading contract: the matching thread NEVER blocks on a client socket.
 * route() encodes and enqueues under the account-stream mutex; the socket
 * write happens on the session's writer thread. A full queue is a slow
 * consumer: the connection is shut down (the reader loop then exits and
 * detaches), the frames are dropped, and GatewayCounters::
 * slowConsumerDisconnects is bumped. The client recovers the gap via
 * ResendRequest after reconnecting.
 *
 * The AccountStream OUTLIVES connections: the outbound sequence number and the
 * bounded resend log persist across a disconnect, so an event that fires while
 * the account is offline is logged and served on reconnect (session-layer
 * recovery). Delivery starts at the account's first attach (an account that
 * never connected has no encoder and its events are not logged).
 *
 * The resend log stores (seq, OutboundEvent, first-send timestamp), NOT
 * encoded frames: frames are (re)encoded per the session's protocol at send
 * and at resend time. SBE encoding is deterministic, so an SBE resend is
 * byte-identical to the original transmission; FIX REQUIRES re-encoding on
 * resend (PossDupFlag 43=Y and OrigSendingTime 122 change BodyLength and
 * CheckSum, so a byte replay was never viable there).
 */
#pragma once

#include "flox-venue/messages.h"
#include "flox-venue/metrics.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue
{

// Wall-clock nanoseconds: the timestamp logged with each outbound event (FIX
// SendingTime 52 at first send, OrigSendingTime 122 on a PossDup resend).
inline int64_t wallClockNs() noexcept
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

class SessionRegistry;

// Gateway hook for session-layer verbs (resend / snapshot): return true when
// the raw inbound frame was a session verb and was fully handled. The SBE
// implementation lives in session_verbs.h.
using SessionVerbHandler = std::function<bool(const uint8_t* frame, size_t n, uint64_t account,
                                              SessionRegistry& registry)>;

// Wire-negotiated per-session configuration (today: cancel-on-disconnect).
// A gateway hook decodes an inbound frame into an update, or nullopt when the
// frame is not a session-config verb; the gateway applies the update to the
// live session. Fire-and-forget by design: the update takes effect
// immediately and has no reply of its own (its effect is observable -- a
// disconnect sweeps or keeps the orders). The SBE decoder lives in
// session_verbs.h (makeSbeSessionConfigVerb).
struct SessionConfigUpdate
{
  bool cancelOnDisconnect{false};
};
using SessionConfigHandler =
    std::function<std::optional<SessionConfigUpdate>(const uint8_t* frame, size_t n)>;

struct DeliveryConfig
{
  size_t queueCapacity{256};       // per-connection outbound frames before slow-consumer disconnect
  size_t resendLogCapacity{1024};  // per-account retained events for ResendRequest
};

// Per-connection outbound writer: a bounded queue drained by a dedicated
// thread. Producers (matching thread, session reader thread) never block on
// the socket; overflow closes the connection.
class SessionWriter
{
 public:
  using WriteFn = std::function<bool(const uint8_t*, size_t)>;
  using CloseFn = std::function<void()>;

  SessionWriter(WriteFn write, CloseFn close, size_t capacity, GatewayCounters* counters = nullptr)
      : write_(std::move(write)), close_(std::move(close)), capacity_(capacity), counters_(counters)
  {
    thread_ = std::thread([this]
                          { run(); });
  }
  ~SessionWriter() { stop(); }

  SessionWriter(const SessionWriter&) = delete;
  SessionWriter& operator=(const SessionWriter&) = delete;

  // Producer side. Never blocks. Returns false if the writer is dead (the
  // frame was dropped). Overflow marks the writer dead, shuts the socket down
  // (the reader loop exits and tears the session down) and bumps the
  // slow-consumer counter exactly once.
  bool enqueue(std::vector<uint8_t> frame)
  {
    CloseFn closeNow;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (dead_ || stopping_)
      {
        return false;
      }
      if (q_.size() >= capacity_)
      {
        dead_ = true;
        closeNow = close_;  // invoke outside the lock
        if (counters_ != nullptr)
        {
          counters_->slowConsumerDisconnects.fetch_add(1, std::memory_order_relaxed);
        }
      }
      else
      {
        q_.push_back(std::move(frame));
      }
    }
    if (closeNow)
    {
      closeNow();
      cv_.notify_one();
      return false;
    }
    cv_.notify_one();
    return true;
  }

  bool dead() const
  {
    std::lock_guard<std::mutex> lk(m_);
    return dead_;
  }

  // Force-kill: mark dead and shut the connection down (used to displace an
  // older connection of the same account). The connection's own reader thread
  // still owns detach() + stop().
  void kill()
  {
    CloseFn c;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (dead_)
      {
        return;
      }
      dead_ = true;
      c = close_;
    }
    cv_.notify_one();
    if (c)
    {
      c();
    }
  }

  // Drain what is queued (best effort unless dead), then join the writer
  // thread. Called by the owning connection on teardown; idempotent.
  void stop()
  {
    {
      std::lock_guard<std::mutex> lk(m_);
      stopping_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable())
    {
      thread_.join();
    }
  }

 private:
  void run()
  {
    std::unique_lock<std::mutex> lk(m_);
    while (true)
    {
      cv_.wait(lk, [this]
               { return stopping_ || dead_ || !q_.empty(); });
      if (dead_)
      {
        q_.clear();
        return;
      }
      if (q_.empty())
      {
        if (stopping_)
        {
          return;  // drained
        }
        continue;
      }
      std::vector<uint8_t> f = std::move(q_.front());
      q_.pop_front();
      lk.unlock();
      const bool ok = write_(f.data(), f.size());
      lk.lock();
      if (!ok)
      {
        dead_ = true;
        q_.clear();
        CloseFn c = close_;
        lk.unlock();
        if (c)
        {
          c();  // peer gone mid-write: make sure the reader unblocks too
        }
        return;
      }
    }
  }

  WriteFn write_;
  CloseFn close_;
  size_t capacity_;
  GatewayCounters* counters_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::vector<uint8_t>> q_;
  bool dead_{false};
  bool stopping_{false};
  std::thread thread_;
};

class SessionRegistry
{
 public:
  // Serialize an event for this account's protocol, stamped with the
  // per-session outbound sequence number; `tsNs` is the wall-clock send time
  // (FIX SendingTime; SBE ignores it). Return false when the event has no wire
  // form for this protocol (nothing is sent or logged).
  using Encoder =
      std::function<bool(const OutboundEvent&, uint64_t seq, int64_t tsNs, std::vector<uint8_t>&)>;
  // One retained outbound event: assigned seq, first-send wall-clock time, and
  // the protocol-agnostic event itself (re-encoded on resend).
  struct Logged
  {
    uint64_t seq;
    int64_t tsNs;
    OutboundEvent event;
  };
  // Observed on the producer thread for every event routed to the account
  // while this connection is attached (terminal-event pruning hooks).
  using EventObserver = std::function<void(const OutboundEvent&)>;

  explicit SessionRegistry(DeliveryConfig cfg = {}, GatewayCounters* counters = nullptr)
      : cfg_(cfg), counters_(counters)
  {
  }

  // Bind a live connection to `account`. A concurrent older connection for the
  // same account is displaced (its socket is shut down; its own reader loop
  // detaches it). The returned writer is owned by the caller (the connection
  // thread), which must detach() and stop() it on teardown.
  std::shared_ptr<SessionWriter> attach(uint64_t account, Encoder encoder,
                                        SessionWriter::WriteFn write, SessionWriter::CloseFn close,
                                        EventObserver observer = {})
  {
    auto stream = streamOf(account, /*create*/ true);
    auto writer =
        std::make_shared<SessionWriter>(std::move(write), std::move(close), cfg_.queueCapacity,
                                        counters_);
    std::shared_ptr<SessionWriter> displaced;
    {
      std::lock_guard<std::mutex> lk(stream->m);
      displaced = std::move(stream->writer);
      stream->writer = writer;
      stream->encoder = std::move(encoder);
      stream->observer = std::move(observer);
    }
    if (displaced)
    {
      // One live connection per account: shut the older one down. Its reader
      // thread unblocks, detaches (a no-op now) and joins its own writer.
      displaced->kill();
    }
    return writer;
  }

  // Unbind `writer` from `account`. After detach returns, no further events or
  // observer calls reach this writer (synchronized on the stream mutex); the
  // seq counter and resend log stay for the next attach.
  void detach(uint64_t account, const std::shared_ptr<SessionWriter>& writer)
  {
    auto stream = streamOf(account, /*create*/ false);
    if (!stream)
    {
      return;
    }
    std::lock_guard<std::mutex> lk(stream->m);
    if (stream->writer == writer)
    {
      stream->writer.reset();
      stream->observer = {};
    }
  }

  // Route one outbound event to the account(s) it belongs to. Called from the
  // matching thread; never blocks on a socket.
  void route(const OutboundEvent& e)
  {
    uint64_t targets[2] = {0, 0};
    int n = 0;
    targetsOf(e, targets, n);
    for (int i = 0; i < n; ++i)
    {
      if (targets[i] == 0)
      {
        continue;  // unrouteable (unbound/trusted-transport account)
      }
      if (i == 1 && targets[1] == targets[0])
      {
        continue;  // self-trade: one report stream, not two
      }
      deliver(targets[i], e);
    }
  }

  // Deliver an event to one specific account (session-level rejects).
  void send(uint64_t account, const OutboundEvent& e) { deliver(account, e); }

  enum class ResendResult : uint8_t
  {
    Served,     // frames (possibly zero: fromSeq > lastSeq) queued to the live writer
    TooOld,     // fromSeq trimmed out of the log -> client needs a snapshot
    NoSession,  // account unknown or no live connection
  };

  // Session-layer recovery: replay logged events with seq >= fromSeq into the
  // account's live writer, re-encoded with their ORIGINAL seqs through the
  // session's encoder. SBE encoding is deterministic, so the replayed frames
  // are byte-identical to the original transmissions.
  ResendResult resendFrom(uint64_t account, uint64_t fromSeq)
  {
    auto stream = streamOf(account, /*create*/ false);
    if (!stream)
    {
      return ResendResult::NoSession;
    }
    std::lock_guard<std::mutex> lk(stream->m);
    if (!stream->writer || !stream->encoder)
    {
      return ResendResult::NoSession;
    }
    if (!stream->log.empty() && fromSeq < stream->log.front().seq)
    {
      return ResendResult::TooOld;
    }
    if (stream->log.empty() && fromSeq <= stream->lastSeq)
    {
      return ResendResult::TooOld;  // everything requested was already trimmed
    }
    for (const auto& logged : stream->log)
    {
      if (logged.seq >= fromSeq)
      {
        std::vector<uint8_t> frame;
        if (stream->encoder(logged.event, logged.seq, logged.tsNs, frame))
        {
          stream->writer->enqueue(std::move(frame));
        }
      }
    }
    if (counters_ != nullptr)
    {
      counters_->resendServed.fetch_add(1, std::memory_order_relaxed);
    }
    return ResendResult::Served;
  }

  // Copy of the retained log entries with fromSeq <= seq <= toSeq (toSeq 0 =
  // no upper bound). Seqs absent from the slice inside that range were either
  // trimmed or never logged (admin messages sent via sendSequencedRaw) -- a
  // FIX resend covers them with SequenceReset-GapFill.
  std::vector<Logged> logSlice(uint64_t account, uint64_t fromSeq, uint64_t toSeq = 0)
  {
    std::vector<Logged> out;
    auto stream = streamOf(account, /*create*/ false);
    if (!stream)
    {
      return out;
    }
    std::lock_guard<std::mutex> lk(stream->m);
    for (const auto& logged : stream->log)
    {
      if (logged.seq >= fromSeq && (toSeq == 0 || logged.seq <= toSeq))
      {
        out.push_back(logged);
      }
    }
    return out;
  }

  // Allocate the next outbound seq and enqueue the frame `build` produces for
  // it, atomically with respect to routed events (same stream mutex, so the
  // wire order matches the seq order). For session-layer messages that consume
  // a seq but are NOT replayable from the event log -- FIX admin traffic
  // (Logon/Heartbeat/TestRequest/ResendRequest/Logout); a later resend covers
  // their seqs with GapFill. Returns false when the account has no live writer
  // or `build` declined.
  using SequencedBuild = std::function<bool(uint64_t seq, int64_t tsNs, std::vector<uint8_t>&)>;
  bool sendSequencedRaw(uint64_t account, const SequencedBuild& build)
  {
    auto stream = streamOf(account, /*create*/ false);
    if (!stream)
    {
      return false;
    }
    std::lock_guard<std::mutex> lk(stream->m);
    if (!stream->writer)
    {
      return false;
    }
    std::vector<uint8_t> frame;
    const uint64_t seq = stream->lastSeq + 1;
    if (!build(seq, wallClockNs(), frame))
    {
      return false;
    }
    stream->lastSeq = seq;
    return stream->writer->enqueue(std::move(frame));
  }

  // Reset the account's outbound stream to seq 1 (FIX Logon with
  // ResetSeqNumFlag 141=Y): the seq counter restarts and the resend log is
  // dropped -- nothing from the previous sequence space may replay into the
  // new one.
  void resetStream(uint64_t account)
  {
    auto stream = streamOf(account, /*create*/ false);
    if (!stream)
    {
      return;
    }
    std::lock_guard<std::mutex> lk(stream->m);
    stream->lastSeq = 0;
    stream->log.clear();
  }

  // Last assigned outbound seq for the account (0 if none).
  uint64_t lastSeq(uint64_t account)
  {
    auto stream = streamOf(account, /*create*/ false);
    if (!stream)
    {
      return 0;
    }
    std::lock_guard<std::mutex> lk(stream->m);
    return stream->lastSeq;
  }

  // Queue a pre-framed session-layer message (snapshot reply, SnapshotRequired)
  // to the account's live writer. Not sequenced, not logged.
  bool enqueueRaw(uint64_t account, std::vector<uint8_t> frame)
  {
    auto stream = streamOf(account, /*create*/ false);
    if (!stream)
    {
      return false;
    }
    std::lock_guard<std::mutex> lk(stream->m);
    return stream->writer && stream->writer->enqueue(std::move(frame));
  }

  // A protocol layer served a resend itself (the FIX 35=2 path replays via
  // enqueueRaw, not resendFrom): account it on the shared counter.
  void noteResendServed()
  {
    if (counters_ != nullptr)
    {
      counters_->resendServed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // ---- outbound-seq persistence (FIX session sidecar) ----
  // The outbound side of a FIX session (per-account lastSeq) survives a venue
  // restart via the checkpoint sidecar (see FixSessionSidecar in
  // fix_session.h). Only the COUNTER is persisted -- the resend log is not:
  // a post-restart resend that reaches into the pre-restart range is answered
  // with SequenceReset-GapFill (the honest signal for history the venue no
  // longer holds; full state reconciliation is the snapshot path).
  // Blob: [u32 count]{u64 account, u64 lastSeq}...
  void serializeSeqs(std::vector<uint8_t>& out)
  {
    std::vector<std::pair<uint64_t, uint64_t>> entries;
    {
      std::lock_guard<std::mutex> lk(m_);
      for (const auto& [account, stream] : streams_)
      {
        std::lock_guard<std::mutex> slk(stream->m);
        if (stream->lastSeq > 0)
        {
          entries.emplace_back(account, stream->lastSeq);
        }
      }
    }
    const uint32_t count = static_cast<uint32_t>(entries.size());
    appendBytes(out, &count, sizeof count);
    for (const auto& [account, lastSeq] : entries)
    {
      appendBytes(out, &account, sizeof account);
      appendBytes(out, &lastSeq, sizeof lastSeq);
    }
  }

  // Restore per-account outbound seq counters into a fresh registry (called
  // before any session attaches). Returns bytes consumed, or 0 on a malformed
  // blob (nothing partially applied before the error is detected).
  size_t restoreSeqs(const uint8_t* p, size_t n)
  {
    uint32_t count = 0;
    if (n < sizeof count)
    {
      return 0;
    }
    std::memcpy(&count, p, sizeof count);
    const size_t need = sizeof count + static_cast<size_t>(count) * 16;
    if (n < need)
    {
      return 0;
    }
    const uint8_t* b = p + sizeof count;
    for (uint32_t i = 0; i < count; ++i, b += 16)
    {
      uint64_t account = 0;
      uint64_t lastSeq = 0;
      std::memcpy(&account, b, sizeof account);
      std::memcpy(&lastSeq, b + 8, sizeof lastSeq);
      auto stream = streamOf(account, /*create*/ true);
      std::lock_guard<std::mutex> lk(stream->m);
      stream->lastSeq = lastSeq;
    }
    return need;
  }

  static void appendBytes(std::vector<uint8_t>& out, const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
  }

 private:
  struct AccountStream
  {
    std::mutex m;
    uint64_t lastSeq{0};
    std::deque<Logged> log;  // seq-ascending, events (not frames): re-encoded on resend
    Encoder encoder;         // sticky across disconnects: offline events keep being logged
    EventObserver observer;
    std::shared_ptr<SessionWriter> writer;  // null while the account is offline
  };

  std::shared_ptr<AccountStream> streamOf(uint64_t account, bool create)
  {
    std::lock_guard<std::mutex> lk(m_);
    auto it = streams_.find(account);
    if (it != streams_.end())
    {
      return it->second;
    }
    if (!create)
    {
      return nullptr;
    }
    auto s = std::make_shared<AccountStream>();
    streams_.emplace(account, s);
    return s;
  }

  void deliver(uint64_t account, const OutboundEvent& e)
  {
    auto stream = streamOf(account, /*create*/ false);
    if (!stream)
    {
      return;  // account never attached: no protocol to serialize with
    }
    std::lock_guard<std::mutex> lk(stream->m);
    if (stream->observer)
    {
      stream->observer(e);
    }
    if (!stream->encoder)
    {
      return;
    }
    std::vector<uint8_t> frame;
    const uint64_t seq = stream->lastSeq + 1;
    const int64_t tsNs = wallClockNs();
    if (!stream->encoder(e, seq, tsNs, frame))
    {
      return;  // no wire form for this event on this protocol
    }
    stream->lastSeq = seq;
    stream->log.push_back(Logged{seq, tsNs, e});
    while (stream->log.size() > cfg_.resendLogCapacity)
    {
      stream->log.pop_front();
    }
    if (stream->writer)
    {
      stream->writer->enqueue(std::move(frame));
    }
  }

  // Owner account(s) of an outbound event. Events without an account concept
  // (none today) or with account 0 are not routed.
  static void targetsOf(const OutboundEvent& e, uint64_t out[2], int& n)
  {
    n = 0;
    if (const auto* a = std::get_if<OrderAccepted>(&e))
    {
      out[n++] = a->account;
    }
    else if (const auto* r = std::get_if<OrderRejected>(&e))
    {
      out[n++] = r->account;
    }
    else if (const auto* t = std::get_if<Trade>(&e))
    {
      out[n++] = t->makerAccount;
      out[n++] = t->takerAccount;
    }
    else if (const auto* x = std::get_if<OrderExecuted>(&e))
    {
      out[n++] = x->account;
    }
    else if (const auto* c = std::get_if<OrderCanceled>(&e))
    {
      out[n++] = c->account;
    }
    else if (const auto* m = std::get_if<OrderModified>(&e))
    {
      out[n++] = m->account;
    }
    else if (const auto* g = std::get_if<OrderTriggered>(&e))
    {
      out[n++] = g->account;
    }
    else if (const auto* fh = std::get_if<FillHeld>(&e))
    {
      out[n++] = fh->makerAccount;
      out[n++] = fh->takerAccount;
    }
    else if (const auto* fr = std::get_if<FillRejected>(&e))
    {
      out[n++] = fr->takerAccount;
      out[n++] = fr->makerAccount;
    }
    else if (const auto* mp = std::get_if<MmpTriggered>(&e))
    {
      out[n++] = mp->accountId;
    }
    else if (const auto* f = std::get_if<FeeCharged>(&e))
    {
      out[n++] = f->account;
    }
    else if (const auto* l = std::get_if<Liquidation>(&e))
    {
      out[n++] = l->account;
    }
    else if (const auto* b = std::get_if<BalanceUpdate>(&e))
    {
      out[n++] = b->account;
    }
  }

  DeliveryConfig cfg_;
  GatewayCounters* counters_;
  std::mutex m_;
  std::unordered_map<uint64_t, std::shared_ptr<AccountStream>> streams_;
};

}  // namespace flox::venue
