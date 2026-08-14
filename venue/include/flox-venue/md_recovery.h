/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * TCP recovery channel for the market-data feed.
 *
 * The multicast feed is fire-and-forget; this server is where a consumer goes
 * when the feed is not enough: a late joiner (no book at all), a detected gap
 * (missed datagrams), or a publisher restart (epoch change). Protocol, one
 * request per connection, length-prefixed frames (flox::net::writeFrame):
 *
 *   client:  ResendRequest{symbol, fromSeq}       (fromSeq=0 -> snapshot)
 *   server:  EITHER the increments with seq >= fromSeq replayed from the
 *            publisher's bounded ring, terminated by EOF,
 *            OR SnapshotRequired + SnapshotBegin + AddOrder body (seq=0,
 *            epoch set) + SnapshotEnd, when fromSeq is older than the ring's
 *            tail. The consumer applies the body and resumes the incremental
 *            feed at lastSeq+1.
 *
 * The snapshot/replay is taken under the publisher's lock (snapshotAtomic /
 * resendFrom), so it is consistent with the live stream. An unknown symbol is
 * answered by EOF with no frames. Stop the server before destroying the
 * publishers registered with it.
 *
 * MdRecoveryClient (below) is the consumer-side counterpart: one call runs the
 * whole round-trip and returns the classified result.
 */
#pragma once

#include "flox-venue/market_data.h"
#include "flox-venue/metrics.h"
#include "flox-venue/resend_buffer.h"
#include "flox-venue/sbe_md_codec.h"
#include "flox-venue/socket_acceptor.h"
#include "flox-venue/udp_multicast.h"
#include "flox/util/transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue
{

class MdRecoveryServer
{
 public:
  explicit MdRecoveryServer(MdCounters* counters = nullptr) : counters_(counters) {}
  ~MdRecoveryServer() { stop(); }

  // Register the recovery source for the publisher's symbol (one per symbol;
  // re-adding replaces, e.g. after a publisher restart). The publisher must
  // outlive the server (or be replaced before it is destroyed).
  template <size_t Levels>
  void addPublisher(MarketDataPublisher<Levels>& pub)
  {
    std::lock_guard<std::mutex> lk(m_);
    sources_[pub.symbol()] =
        Source{[&pub]
               { return pub.snapshotAtomic(); },
               [&pub](uint64_t fromSeq)
               { return pub.resendFrom(fromSeq); }};
  }

  // Listen on bindIp:port (0 = ephemeral). Returns the bound port, or -1.
  // bindIp nullptr/"" keeps the loopback-only default. Passing an explicit
  // address ("0.0.0.0" / a NIC address) exposes the recovery channel beyond
  // the host; the hardening contract is the same as for the order-entry
  // gateways -- put TLS termination and/or a firewall in front before binding
  // externally, the protocol itself carries no authentication or encryption
  // (see docs/venue/market-data.md).
  int start(uint16_t port, const char* bindIp = nullptr)
  {
    return acceptor_.start(port, [this](int fd)
                           { serve(fd); }, bindIp);
  }

  void stop() { acceptor_.stop(); }
  int port() const noexcept { return acceptor_.port(); }

 private:
  struct Source
  {
    std::function<MdSnapshot()> snapshot;
    std::function<std::optional<std::vector<MdMessage>>(uint64_t)> resend;
  };

  void serve(int fd)
  {
    std::vector<uint8_t> frame;
    if (!net::readFrame(fd, frame))
    {
      return;
    }
    MdResendRequest req{};
    if (!SbeMdCodec::decode(frame.data(), frame.size(), req))
    {
      return;
    }
    Source src;
    {
      std::lock_guard<std::mutex> lk(m_);
      const auto it = sources_.find(req.symbol);
      if (it == sources_.end())
      {
        return;  // unknown symbol: EOF, no frames
      }
      src = it->second;
    }

    std::vector<uint8_t> buf;
    if (const auto tail = src.resend(req.fromSeq))
    {
      for (const MdMessage& m : *tail)
      {
        SbeMdCodec::encode(m, buf);
        if (!net::writeFrame(fd, buf.data(), buf.size()))
        {
          return;
        }
      }
      if (counters_ != nullptr)
      {
        counters_->resendServed.fetch_add(1, std::memory_order_relaxed);
      }
      return;  // EOF terminates the replay
    }

    // fromSeq is older than the ring's tail (or 0): the increments are gone,
    // serve the whole book instead.
    const MdSnapshot snap = src.snapshot();
    SbeMdCodec::encode(MdSnapshotRequired{req.symbol, snap.epoch, snap.lastSeq}, buf);
    if (!net::writeFrame(fd, buf.data(), buf.size()))
    {
      return;
    }
    SbeMdCodec::encode(
        MdSnapshotBegin{req.symbol, snap.epoch, snap.lastSeq,
                        static_cast<uint32_t>(snap.orders.size())},
        buf);
    if (!net::writeFrame(fd, buf.data(), buf.size()))
    {
      return;
    }
    // Current trading status and derivatives layer BEFORE the book body: a
    // consumer that reconnects into a halted or paused instrument must not have
    // to guess it from the absence of increments (SnapshotBegin's orderCount
    // still counts only the AddOrder body).
    if (snap.hasStatus)
    {
      SbeMdCodec::encode(snap.status, buf);
      if (!net::writeFrame(fd, buf.data(), buf.size()))
      {
        return;
      }
    }
    if (snap.hasDerivatives)
    {
      SbeMdCodec::encode(snap.derivatives, buf);
      if (!net::writeFrame(fd, buf.data(), buf.size()))
      {
        return;
      }
    }
    for (const MdMessage& m : snap.orders)
    {
      SbeMdCodec::encode(m, buf);
      if (!net::writeFrame(fd, buf.data(), buf.size()))
      {
        return;
      }
    }
    SbeMdCodec::encode(MdSnapshotEnd{req.symbol, snap.epoch, snap.lastSeq}, buf);
    if (!net::writeFrame(fd, buf.data(), buf.size()))
    {
      return;
    }
    if (counters_ != nullptr)
    {
      counters_->snapshotsServed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  MdCounters* counters_;
  SocketAcceptor acceptor_;
  std::mutex m_;
  std::unordered_map<SymbolId, Source> sources_;
};

// Consumer-side client for the recovery channel above. One recover() call is
// one TCP connection (the protocol is one request per connection), so the
// class holds no socket state and its destructor has nothing to fail. The
// consumer protocol it packages: on a detected gap or an epoch change, ask for
// `fromSeq` (0 = late joiner / re-snapshot), apply either the replayed
// increments or the snapshot body, reset the GapDetector to `lastSeq + 1`
// (`resetSequencer`) and resume the incremental feed.
class MdRecoveryClient
{
 public:
  enum class Status : uint8_t
  {
    Ok,             // result populated (possibly zero increments: nothing past fromSeq)
    ConnectError,   // could not reach the server (or the request failed to send)
    Timeout,        // server reachable but the reply stalled past the socket timeout
    ProtocolError,  // malformed / truncated reply (e.g. a snapshot without SnapshotEnd)
  };

  struct Result
  {
    bool snapshot{false};             // true = SnapshotRequired path (`messages` is the book body)
    std::vector<MdMessage> messages;  // replayed increments, or AddOrder body records (seq=0)
    MdSnapshotBegin begin{};          // valid when `snapshot`
    MdSnapshotEnd end{};              // valid when `snapshot`
    // Instrument state carried by a snapshot, kept OUT of `messages` so that
    // vector stays exactly what it has always been -- the book body.
    bool hasStatus{false};
    MdMessage status{};
    bool hasDerivatives{false};
    MdMessage derivatives{};
    // Resume point: the incremental feed continues at lastSeq + 1. On the
    // snapshot path these come from SnapshotEnd; on a replay, from the last
    // increment. An EMPTY replay (fromSeq ahead of the stream) keeps the
    // consumer's own position: lastSeq = fromSeq - 1, epoch = 0 (unchanged).
    uint64_t lastSeq{0};
    uint64_t epoch{0};
  };

  MdRecoveryClient(std::string host, uint16_t port,
                   std::chrono::milliseconds timeout = std::chrono::seconds(2))
      : host_(std::move(host)), port_(port), timeout_(timeout)
  {
  }

  Status recover(SymbolId symbol, uint64_t fromSeq, Result& out) const
  {
    out = Result{};
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
      return Status::ConnectError;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &a.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0)
    {
      ::close(fd);
      return Status::ConnectError;
    }
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout_.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout_.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    std::vector<uint8_t> b;
    SbeMdCodec::encode(MdResendRequest{symbol, fromSeq}, b);
    if (!net::writeFrame(fd, b.data(), b.size()))
    {
      ::close(fd);
      return Status::ConnectError;
    }

    const Status st = collect(fd, fromSeq, out);
    ::close(fd);
    return st;
  }

 private:
  Status collect(int fd, uint64_t fromSeq, Result& out) const
  {
    bool sawBegin = false;
    bool sawEnd = false;
    std::vector<uint8_t> f;
    while (true)
    {
      errno = 0;
      if (!net::readFrame(fd, f))
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return Status::Timeout;  // reply stalled mid-stream, not a clean EOF
        }
        break;  // EOF terminates a replay (and follows SnapshotEnd)
      }
      switch (static_cast<SbeMdCodec::Tmpl>(SbeMdCodec::templateId(f.data(), f.size())))
      {
        case SbeMdCodec::Tmpl::SnapshotRequired:
        {
          MdSnapshotRequired sr{};
          if (!SbeMdCodec::decode(f.data(), f.size(), sr))
          {
            return Status::ProtocolError;
          }
          out.snapshot = true;
          break;
        }
        case SbeMdCodec::Tmpl::SnapshotBegin:
          sawBegin = SbeMdCodec::decode(f.data(), f.size(), out.begin);
          break;
        case SbeMdCodec::Tmpl::SnapshotEnd:
          sawEnd = SbeMdCodec::decode(f.data(), f.size(), out.end);
          break;
        default:
        {
          MdMessage m;
          if (!SbeMdCodec::decode(f.data(), f.size(), m))
          {
            return Status::ProtocolError;
          }
          // Inside a snapshot these two are the instrument's CURRENT state
          // (seq=0), not book records, so they are lifted out of the body. On a
          // replay they are ordinary sequenced increments and must stay in the
          // stream -- diverting them there would punch a hole in the seq run.
          if (out.snapshot && m.type == MdType::TradingStatus)
          {
            out.hasStatus = true;
            out.status = m;
          }
          else if (out.snapshot && m.type == MdType::DerivativesUpdate)
          {
            out.hasDerivatives = true;
            out.derivatives = m;
          }
          else
          {
            out.messages.push_back(m);
          }
          break;
        }
      }
      if (sawEnd)
      {
        break;  // snapshot fully served; the server closes after this anyway
      }
    }
    if (out.snapshot)
    {
      if (!sawBegin || !sawEnd)
      {
        return Status::ProtocolError;  // truncated snapshot must never look complete
      }
      out.lastSeq = out.end.lastSeq;
      out.epoch = out.end.epoch;
      return Status::Ok;
    }
    if (sawBegin || sawEnd)
    {
      return Status::ProtocolError;  // snapshot frames without SnapshotRequired
    }
    if (!out.messages.empty())
    {
      out.lastSeq = out.messages.back().seq;
      out.epoch = out.messages.back().epoch;
    }
    else
    {
      out.lastSeq = fromSeq > 0 ? fromSeq - 1 : 0;  // nothing new: keep the position
    }
    return Status::Ok;
  }

  std::string host_;
  uint16_t port_;
  std::chrono::milliseconds timeout_;
};

// Self-recovering market-data consumer: UdpMdSubscriber (raw datagrams) +
// GapDetector (owned here) + MdRecoveryClient composed into the full consumer
// protocol, so a subscriber neither implements gap accounting nor the TCP
// round-trips itself. recv()-style API, mirroring UdpMdSubscriber::recv with a
// detector attached: only in-order messages surface, recovery is invisible
// except through the onSnapshot callback and the counters.
//
// - Gap: ResendRequest{fromSeq} on the recovery channel; the replayed
//   increments are woven through the detector into the in-order stream.
// - Epoch change (publisher restart) or a gap deeper than
//   maxGapBeforeSnapshot: snapshot recovery (fromSeq=0) -- the snapshot body
//   is handed to the onSnapshot callback (apply it to your book), the
//   sequencer fast-forwards to lastSeq+1 and the incremental feed continues.
// - A failed round-trip retries with doubling backoff, bounded by
//   maxAttempts -- never an eternal loop; exhaustion counts in
//   recoveriesFailed() and the stream falls back to plain gap semantics
//   (the detector re-raises or abandons per its own config).
//
// The wrapped subscriber must NOT have its own GapDetector attached
// (setGapDetector): this class owns sequencing. Single-threaded like the
// subscriber itself: all recovery work happens inside recv() on the calling
// thread (backoff sleeps block the caller, as any synchronous recover would).
class RecoveringMdSubscriber
{
 public:
  struct Config
  {
    std::string host{"127.0.0.1"};  // recovery server (MdRecoveryServer)
    uint16_t port{0};
    uint64_t maxGapBeforeSnapshot{256};                                       // deeper holes take the snapshot path
    int maxAttempts{3};                                                       // recovery round-trips per incident
    std::chrono::milliseconds initialBackoff{std::chrono::milliseconds(20)};  // doubles per retry
  };

  // Snapshot application hook: `orders` is the AddOrder book body (seq=0).
  // Clear the symbol's book and apply the records; the wrapper resumes the
  // incremental stream at begin.lastSeq + 1 afterwards.
  using SnapshotFn = std::function<void(SymbolId symbol, const MdSnapshotBegin& begin,
                                        const std::vector<MdMessage>& orders)>;

  // Instrument state carried by the same snapshot: the CURRENT trading status
  // and derivatives layer (null when the publisher has not published one yet).
  // Separate from SnapshotFn because these are not book records -- a consumer
  // that only rebuilds a book ignores them, one that tracks halts or values a
  // perp position takes them before applying the body.
  using SnapshotStateFn =
      std::function<void(SymbolId symbol, const MdMessage* status, const MdMessage* derivatives)>;

  RecoveringMdSubscriber(UdpMdSubscriber& sub, Config cfg, GapDetector::Config gdCfg = {})
      : sub_(sub), cfg_(std::move(cfg)), gd_(gdCfg)
  {
  }

  void onSnapshot(SnapshotFn fn) { onSnapshot_ = std::move(fn); }
  void onSnapshotState(SnapshotStateFn fn) { onSnapshotState_ = std::move(fn); }

  // Receive the next IN-ORDER message; false on socket timeout / error with
  // no recovery pending (a pending recovery is serviced before giving up).
  bool recv(MdMessage& out)
  {
    while (true)
    {
      if (recoveryPending())
      {
        serviceRecovery();
      }
      if (!ready_.empty())
      {
        out = ready_.front();
        ready_.pop_front();
        return true;
      }
      MdMessage m;
      if (!sub_.recv(m))
      {
        if (!recoveryPending())
        {
          return false;  // plain timeout, nothing to recover
        }
        continue;  // service the pending recovery (bounded), then re-check
      }
      observe(m);
    }
  }

  // Next seq the symbol's stream will deliver (GapDetector::expected).
  uint64_t expected(SymbolId symbol) const { return gd_.expected(symbol); }

  uint64_t gapsDetected() const noexcept { return gaps_; }
  uint64_t epochChanges() const noexcept { return epochs_; }
  uint64_t resendsRecovered() const noexcept { return resends_; }
  uint64_t snapshotsRecovered() const noexcept { return snapshots_; }
  uint64_t recoveriesFailed() const noexcept { return failures_; }

 private:
  struct Pending
  {
    bool active{false};
    bool snapshot{false};  // snapshot path (epoch change / too-deep gap) vs resend
    uint64_t fromSeq{0};
    uint64_t epoch{0};
    int attempts{0};
  };

  bool recoveryPending() const
  {
    for (const auto& [sym, p] : pending_)
    {
      (void)sym;
      if (p.active)
      {
        return true;
      }
    }
    return false;
  }

  void deliver(const MdMessage& m) { ready_.push_back(m); }

  void observe(const MdMessage& m)
  {
    gd_.observe(m, [this](const MdMessage& d)
                { deliver(d); }, [this](SymbolId s, uint64_t ep, uint64_t from)
                {
                  ++gaps_;
                  Pending& p = pending_[s];
                  if (!p.active)
                  {
                    p = Pending{};
                    p.active = true;
                  }
                  p.fromSeq = from;
                  p.epoch = ep; }, [this](SymbolId s, uint64_t, uint64_t newEpoch)
                {
                  ++epochs_;
                  Pending& p = pending_[s];
                  p = Pending{};
                  p.active = true;
                  p.snapshot = true;  // prior book is void, not merely gapped
                  p.epoch = newEpoch; });
    // Depth check outside the callbacks: this message's seq bounds the hole.
    // A gap deeper than the resend budget flips to the snapshot path -- the
    // server would route it there anyway once the ring has trimmed.
    auto it = pending_.find(m.symbol);
    if (it != pending_.end() && it->second.active && !it->second.snapshot &&
        m.seq > it->second.fromSeq && m.seq - it->second.fromSeq > cfg_.maxGapBeforeSnapshot)
    {
      it->second.snapshot = true;
    }
  }

  void serviceRecovery()
  {
    for (auto& [sym, p] : pending_)
    {
      if (p.active)
      {
        attemptRecovery(sym, p);
      }
    }
  }

  void attemptRecovery(SymbolId sym, Pending& p)
  {
    MdRecoveryClient::Result res;
    const uint64_t fromSeq = p.snapshot ? 0 : p.fromSeq;
    const MdRecoveryClient::Status st =
        MdRecoveryClient(cfg_.host, cfg_.port).recover(sym, fromSeq, res);
    if (st != MdRecoveryClient::Status::Ok)
    {
      ++p.attempts;
      if (p.attempts >= cfg_.maxAttempts)
      {
        ++failures_;
        p = Pending{};  // give up on this incident; the detector re-raises or abandons
        return;
      }
      // Bounded, doubling backoff between attempts; the next recv() pass
      // retries. Never an unbounded spin: attempts are capped above.
      std::this_thread::sleep_for(cfg_.initialBackoff * (1 << (p.attempts - 1)));
      return;
    }
    if (res.snapshot)
    {
      // Server chose (or we requested) the snapshot path. Hand the book body
      // to the consumer, then fast-forward the sequencer: held datagrams past
      // lastSeq drain into the in-order stream immediately.
      if (onSnapshotState_)
      {
        onSnapshotState_(sym, res.hasStatus ? &res.status : nullptr,
                         res.hasDerivatives ? &res.derivatives : nullptr);
      }
      if (onSnapshot_)
      {
        onSnapshot_(sym, res.begin, res.messages);
      }
      gd_.reset(sym, res.epoch, res.lastSeq + 1, [this](const MdMessage& d)
                { deliver(d); });
      ++snapshots_;
    }
    else
    {
      // Replayed increments weave through the detector: they fill the hole in
      // seq order and drain whatever was held behind it. If the replay did
      // not reach the hole's end, the still-open gap re-raises on subsequent
      // live traffic and recovery re-arms.
      for (const MdMessage& m : res.messages)
      {
        gd_.observe(m, [this](const MdMessage& d)
                    { deliver(d); });
      }
      ++resends_;
    }
    p = Pending{};
  }

  UdpMdSubscriber& sub_;
  Config cfg_;
  GapDetector gd_;
  SnapshotFn onSnapshot_;
  SnapshotStateFn onSnapshotState_;
  std::deque<MdMessage> ready_;  // sequenced, deliverable messages
  std::unordered_map<SymbolId, Pending> pending_;
  uint64_t gaps_{0};
  uint64_t epochs_{0};
  uint64_t resends_{0};
  uint64_t snapshots_{0};
  uint64_t failures_{0};
};

}  // namespace flox::venue
