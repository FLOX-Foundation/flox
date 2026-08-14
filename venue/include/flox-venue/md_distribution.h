/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Unicast (TCP) market-data distribution.
 *
 * The multicast feed (udp_multicast.h) is the right feed for a consumer that
 * sits inside the same layer-2 segment, and an impossible one for anybody
 * else: multicast does not survive a WAN hop, most cloud networks do not route
 * it, and a consumer behind NAT cannot join a group. This server is the same
 * feed over an ordinary TCP connection, so an external consumer can take it
 * from anywhere the venue is reachable.
 *
 * Nothing about sequencing is reinvented here. MarketDataPublisher stays the
 * single source of truth -- per-message seq, the publisher epoch, the atomic
 * (snapshot, lastSeq) pair and the bounded resend ring -- and this server is a
 * fan-out in front of it. A subscriber on this channel and a subscriber on the
 * multicast channel see the same messages with the same seq under the same
 * epoch.
 *
 *   engine -> MarketDataPublisher -> sink
 *                                     |-> UdpMdPublisher      (colocated)
 *                                     `-> MdDistributionServer::publish
 *                                            -> per-session encode
 *                                               -> SessionWriter (bounded queue)
 *                                                  -> writer thread -> socket
 *
 * Threading contract, the same one the order-entry delivery path holds: the
 * matching thread NEVER blocks on a subscriber's socket. publish() encodes and
 * hands the frame to the session's bounded queue; the write happens on that
 * session's writer thread. A full queue is a slow consumer -- the connection is
 * shut down, the frames are dropped and MdCounters::slowConsumerDisconnects is
 * bumped. One slow subscriber cannot delay another and cannot delay the engine.
 * (The one section where publish() waits on a session is the handover at the
 * end of a subscribe: the snapshot frames are queued under the session mutex so
 * they cannot be overtaken. That is a bounded, CPU-only stretch proportional to
 * the book size -- never a socket wait.)
 *
 * Subscription handover. A snapshot taken while the feed keeps moving must
 * neither lose an increment nor let one jump ahead of the book it belongs to.
 * The session therefore registers the symbol UNARMED first: from that instant
 * every increment for it is encoded and parked in a bounded per-symbol buffer.
 * The snapshot is then taken outside the session lock (it takes the publisher's
 * lock, which the fan-out holds while calling in -- taking both in that order
 * would deadlock). Finally, under the session lock, the snapshot frames are
 * queued, the parked increments past lastSeq are queued behind them, and the
 * subscription arms at lastSeq+1.
 *
 * Resend on the same connection: ResendRequest{symbol, fromSeq} replays the
 * increments from the publisher's ring; when fromSeq is older than the ring's
 * tail the answer is the explicit "you need a snapshot" message followed by
 * one -- the same two-way answer the recovery channel gives, inline in the
 * session instead of on a connection of its own.
 *
 * Publisher restart: a message whose epoch differs from the subscription's
 * resets the delivery position, so a feed that restarted at seq=1 is not
 * filtered out as stale by a subscription still expecting seq=500. The
 * subscriber sees the epoch change and re-subscribes.
 *
 * Encoding is chosen per connection by its FIRST BYTE, with no preamble of the
 * venue's own invention:
 *
 *   0x00 -> SBE   (the high byte of the u32 frame length; feed frames are tens
 *                  of bytes, so it is always zero)
 *   '8'  -> FIX   (the first byte of the BeginString "8=FIX.4.4")
 *
 * The byte is peeked, not consumed, so both encodings speak their own protocol
 * from their own first byte and an off-the-shelf implementation of either
 * connects unmodified. One port serves both. registerEncoding() adds a third
 * without touching a line of distribution logic, and MdDistributionConfig::
 * encoding pins a port to one encoding when a deployment prefers that.
 *
 * Hardening: like the recovery channel and the order-entry gateways, this
 * protocol carries no authentication and no encryption of its own. start()
 * binds loopback unless given an explicit address; anything reachable off-host
 * belongs behind TLS termination and/or a firewall. See
 * docs/venue/market-data.md.
 */
#pragma once

#include "flox-venue/fix_md_codec.h"
#include "flox-venue/market_data.h"
#include "flox-venue/md_encoder.h"
#include "flox-venue/metrics.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/socket_acceptor.h"

#include "flox/util/transport.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue
{

enum class MdWireEncoding : uint8_t
{
  Auto,  // pick per connection from its first byte
  Sbe,
  Fix,
};

struct MdDistributionConfig
{
  size_t queueCapacity{4096};    // queued outbound writes before a subscriber is dropped
  size_t pendingCapacity{4096};  // increments parked per symbol while its snapshot is in flight
  int readTimeoutMs{200};        // socket read timeout: also the liveness tick
  int idleTimeoutMs{30000};      // no inbound byte for this long -> drop the subscriber
  int heartbeatMs{5000};         // send a heartbeat when the connection has been quiet
  int sendBufferBytes{0};        // SO_SNDBUF on the accepted socket; 0 = kernel default
  MdWireEncoding encoding{MdWireEncoding::Auto};
};

class MdDistributionServer
{
 public:
  using EncoderFactory = std::function<std::unique_ptr<MdEncoder>()>;

  explicit MdDistributionServer(MdCounters* counters = nullptr, MdDistributionConfig cfg = {})
      : counters_(counters), cfg_(cfg)
  {
    encodings_[SbeMdEncoder::kLeadByte] = []
    { return std::unique_ptr<MdEncoder>(new SbeMdEncoder()); };
    encodings_[FixMdEncoder::kLeadByte] = [this]
    { return std::unique_ptr<MdEncoder>(new FixMdEncoder(fixCfg_)); };
  }

  ~MdDistributionServer() { stop(); }

  MdDistributionServer(const MdDistributionServer&) = delete;
  MdDistributionServer& operator=(const MdDistributionServer&) = delete;

  // Register the feed source for the publisher's symbol (one per symbol;
  // re-adding replaces, e.g. after a publisher restart). The publisher must
  // outlive the server, or be replaced before it is destroyed.
  template <size_t Levels>
  void addPublisher(MarketDataPublisher<Levels>& pub)
  {
    std::lock_guard<std::mutex> lk(sourcesMutex_);
    sources_[pub.symbol()] = Source{[&pub]
                                    { return pub.snapshotAtomic(); },
                                    [&pub](uint64_t fromSeq)
                                    { return pub.resendFrom(fromSeq); }};
  }

  // Add (or replace) an encoding, keyed by the first byte a client of it
  // sends. Distribution logic is untouched by this.
  void registerEncoding(uint8_t leadByte, EncoderFactory factory)
  {
    std::lock_guard<std::mutex> lk(sourcesMutex_);
    encodings_[leadByte] = std::move(factory);
  }

  // CompIDs for the built-in FIX encoding. Must be set before start().
  void setFixConfig(FixMdConfig cfg) { fixCfg_ = std::move(cfg); }

  // Listen on bindIp:port (0 = ephemeral). Returns the bound port, or -1.
  // bindIp nullptr/"" keeps the loopback-only default; an explicit address
  // ("0.0.0.0", a NIC address) exposes the feed beyond the host under the
  // hardening contract in the file comment.
  int start(uint16_t port, const char* bindIp = nullptr)
  {
    return acceptor_.start(port, [this](int fd)
                           { serve(fd); }, bindIp);
  }

  void stop() { acceptor_.stop(); }
  int port() const noexcept { return acceptor_.port(); }

  // Fan one published increment out to every subscriber of its symbol. Called
  // from the MarketDataPublisher sink, i.e. on the matching thread: it encodes
  // and enqueues, and never waits on a socket.
  void publish(const MdMessage& m)
  {
    std::shared_lock<std::shared_mutex> lk(sessionsMutex_);
    for (const auto& s : sessions_)
    {
      s->onIncrement(m);
    }
  }

  size_t subscriberCount() const
  {
    std::shared_lock<std::shared_mutex> lk(sessionsMutex_);
    return sessions_.size();
  }

 private:
  struct Source
  {
    std::function<MdSnapshot()> snapshot;
    std::function<std::optional<std::vector<MdMessage>>(uint64_t)> resend;
  };

  bool source(SymbolId symbol, Source& out) const
  {
    std::lock_guard<std::mutex> lk(sourcesMutex_);
    const auto it = sources_.find(symbol);
    if (it == sources_.end())
    {
      return false;
    }
    out = it->second;
    return true;
  }

  // One connected subscriber: its encoder, its bounded outbound queue and its
  // per-symbol subscriptions. Everything that touches the encoder or the queue
  // runs under m_, so the reader thread and the matching thread never race and
  // the wire order matches the order decisions were made in.
  class Session
  {
   public:
    Session(MdDistributionServer& server, std::unique_ptr<MdEncoder> enc, int fd)
        : server_(server), enc_(std::move(enc))
    {
      writer_ = std::make_shared<SessionWriter>(
          [fd](const uint8_t* p, size_t n)
          { return net::writeAll(fd, p, n); }, [fd]
          { ::shutdown(fd, SHUT_RDWR); }, server_.cfg_.queueCapacity, &writerCounters_);
    }

    ~Session() { writer_->stop(); }

    const char* encodingName() const noexcept { return enc_->name(); }
    bool dead() const { return writer_->dead(); }
    void stop() { writer_->stop(); }

    // Producer side (matching thread).
    void onIncrement(const MdMessage& m)
    {
      std::lock_guard<std::mutex> lk(m_);
      const auto it = subs_.find(m.symbol);
      if (it == subs_.end())
      {
        return;
      }
      Sub& s = it->second;
      scratch_.clear();
      enc_->increment(m, scratch_);
      if (scratch_.empty())
      {
        return;  // the encoding has no form for this message
      }
      if (!s.armed)
      {
        if (s.pending.size() >= server_.cfg_.pendingCapacity)
        {
          // The subscriber cannot even absorb its own snapshot handover: the
          // same slow-consumer verdict, reached one step earlier.
          writer_->kill();
          if (!slowCounted_)
          {
            slowCounted_ = true;
            if (server_.counters_ != nullptr)
            {
              server_.counters_->slowConsumerDisconnects.fetch_add(1, std::memory_order_relaxed);
            }
          }
          return;
        }
        s.pending.emplace_back(m.seq, scratch_);
        return;
      }
      if (s.epoch == 0)
      {
        s.epoch = m.epoch;
      }
      else if (m.epoch != s.epoch)
      {
        // The publisher restarted: seq began again at 1 under a new epoch, so
        // the old delivery position would silently swallow the new stream.
        s.epoch = m.epoch;
        s.deliverFrom = m.seq;
      }
      if (m.seq < s.deliverFrom)
      {
        return;  // already covered by the snapshot or the replay
      }
      enqueueLocked(scratch_);
    }

    // Consumer side (this connection's reader thread).
    void handle(const MdRequest& req)
    {
      switch (req.kind)
      {
        case MdRequestKind::Subscribe:
        case MdRequestKind::Resend:
          armFrom(req.symbol, req.fromSeq, req.snapshotOnly);
          break;
        case MdRequestKind::Unsubscribe:
        {
          std::lock_guard<std::mutex> lk(m_);
          subs_.erase(req.symbol);
          break;
        }
        case MdRequestKind::None:
        case MdRequestKind::Close:
          break;
      }
    }

    void enqueue(const std::vector<uint8_t>& frames)
    {
      if (frames.empty())
      {
        return;
      }
      std::lock_guard<std::mutex> lk(m_);
      enqueueLocked(frames);
    }

    void sendHeartbeat()
    {
      std::lock_guard<std::mutex> lk(m_);
      scratch_.clear();
      enc_->heartbeat(scratch_);
      enqueueLocked(scratch_);
    }

    // Decode one inbound message. The encoder is session state, so parsing is
    // serialized with the fan-out like every other use of it.
    MdEncoder::Parse parse(const uint8_t* in, size_t n, size_t& consumed,
                           std::vector<MdRequest>& out, std::vector<uint8_t>& reply)
    {
      std::lock_guard<std::mutex> lk(m_);
      return enc_->parse(in, n, consumed, out, reply);
    }

   private:
    struct Sub
    {
      bool armed{false};                                               // false while the snapshot for this symbol is in flight
      uint64_t deliverFrom{0};                                         // first seq this subscriber still needs
      uint64_t epoch{0};                                               // publisher lifetime the position belongs to
      std::vector<std::pair<uint64_t, std::vector<uint8_t>>> pending;  // parked during handover
    };

    // Subscribe, resume or resend -- one path, because they are one thing:
    // "put this subscriber at fromSeq and keep it there". fromSeq 0 (or older
    // than the ring) means the snapshot; otherwise the ring replay.
    void armFrom(SymbolId symbol, uint64_t fromSeq, bool snapshotOnly)
    {
      {
        // Park increments from this instant on, so the window between here and
        // the handover below loses nothing.
        std::lock_guard<std::mutex> lk(m_);
        Sub& s = subs_[symbol];
        s.armed = false;
        s.pending.clear();
      }

      Source src;
      if (!server_.source(symbol, src))
      {
        std::lock_guard<std::mutex> lk(m_);
        subs_.erase(symbol);
        scratch_.clear();
        enc_->reject(symbol, MdRejectReason::UnknownSymbol, scratch_);
        enqueueLocked(scratch_);
        if (server_.counters_ != nullptr)
        {
          server_.counters_->subscribeRejects.fetch_add(1, std::memory_order_relaxed);
        }
        return;
      }

      // Outside the session lock: these take the publisher's lock, and the
      // fan-out calls in while holding it.
      std::optional<std::vector<MdMessage>> replay;
      if (fromSeq > 0)
      {
        replay = src.resend(fromSeq);
      }
      std::optional<MdSnapshot> snap;
      if (!replay.has_value())
      {
        snap = src.snapshot();
      }

      std::lock_guard<std::mutex> lk(m_);
      const auto it = subs_.find(symbol);
      if (it == subs_.end())
      {
        return;  // unsubscribed while the snapshot was being taken
      }
      Sub& s = it->second;
      uint64_t deliverFrom = 0;
      uint64_t epoch = 0;
      scratch_.clear();
      if (replay.has_value())
      {
        for (const MdMessage& m : *replay)
        {
          enc_->increment(m, scratch_);
        }
        deliverFrom = replay->empty() ? fromSeq : replay->back().seq + 1;
        epoch = replay->empty() ? 0 : replay->back().epoch;
        if (server_.counters_ != nullptr)
        {
          server_.counters_->resendServed.fetch_add(1, std::memory_order_relaxed);
        }
      }
      else
      {
        if (fromSeq > 0)
        {
          // The increments asked for have been trimmed: say so explicitly
          // before the book that replaces them.
          enc_->snapshotRequired(symbol, snap->epoch, snap->lastSeq, scratch_);
        }
        enc_->snapshot(symbol, *snap, scratch_);
        deliverFrom = snap->lastSeq + 1;
        epoch = snap->epoch;
        if (server_.counters_ != nullptr)
        {
          server_.counters_->snapshotsServed.fetch_add(1, std::memory_order_relaxed);
        }
      }
      enqueueLocked(scratch_);
      for (auto& [seq, frame] : s.pending)
      {
        if (seq >= deliverFrom)
        {
          enqueueLocked(frame);
        }
      }
      s.pending.clear();
      s.armed = true;
      s.deliverFrom = deliverFrom;
      s.epoch = epoch;
      if (snapshotOnly)
      {
        subs_.erase(it);  // one book, no stream
      }
    }

    void enqueueLocked(const std::vector<uint8_t>& frames)
    {
      if (frames.empty())
      {
        return;
      }
      writer_->enqueue(frames);
      noteWriterLocked();
    }

    // SessionWriter latches the overflow verdict once per connection into its
    // own counters; mirror that single transition onto the feed counters.
    void noteWriterLocked()
    {
      if (slowCounted_ ||
          writerCounters_.slowConsumerDisconnects.load(std::memory_order_relaxed) == 0)
      {
        return;
      }
      slowCounted_ = true;
      if (server_.counters_ != nullptr)
      {
        server_.counters_->slowConsumerDisconnects.fetch_add(1, std::memory_order_relaxed);
      }
    }

    MdDistributionServer& server_;
    mutable std::mutex m_;
    std::unique_ptr<MdEncoder> enc_;
    std::shared_ptr<SessionWriter> writer_;
    GatewayCounters writerCounters_;  // per-connection, so the overflow verdict is exact
    std::unordered_map<SymbolId, Sub> subs_;
    std::vector<uint8_t> scratch_;  // encode buffer, guarded by m_
    bool slowCounted_{false};
  };

  // Pick the encoding for a fresh connection from its first byte WITHOUT
  // consuming it, so each protocol still starts at its own first byte.
  std::unique_ptr<MdEncoder> makeEncoder(int fd)
  {
    uint8_t lead = 0;
    switch (cfg_.encoding)
    {
      case MdWireEncoding::Sbe:
        lead = SbeMdEncoder::kLeadByte;
        break;
      case MdWireEncoding::Fix:
        lead = FixMdEncoder::kLeadByte;
        break;
      case MdWireEncoding::Auto:
      {
        // The socket carries a read timeout (it is also the liveness tick), so
        // wait for the first byte across ticks rather than dropping a client
        // that connects a moment before it speaks.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.idleTimeoutMs);
        ssize_t r = -1;
        while (acceptor_.running() && std::chrono::steady_clock::now() < deadline)
        {
          r = ::recv(fd, &lead, 1, MSG_PEEK);
          if (r == 1)
          {
            break;
          }
          if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
          {
            return nullptr;
          }
        }
        if (r != 1)
        {
          return nullptr;
        }
        break;
      }
    }
    std::lock_guard<std::mutex> lk(sourcesMutex_);
    const auto it = encodings_.find(lead);
    return it == encodings_.end() ? nullptr : it->second();
  }

  void serve(int fd)
  {
    timeval tv{};
    tv.tv_sec = cfg_.readTimeoutMs / 1000;
    tv.tv_usec = (cfg_.readTimeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    // Deliberately NO send timeout. The writer thread is allowed to block on a
    // wedged peer -- that is what it is for. The slow-consumer verdict belongs
    // to the venue's own bounded queue, not to a socket timer that would
    // misreport a slow reader as a broken connection. Bounding SO_SNDBUF keeps
    // the kernel from hiding backlog the queue is supposed to see.
    if (cfg_.sendBufferBytes > 0)
    {
      const int sz = cfg_.sendBufferBytes;
      ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    }

    auto enc = makeEncoder(fd);
    if (!enc)
    {
      return;  // unknown encoding, or the peer never sent its first byte
    }

    auto session = std::make_shared<Session>(*this, std::move(enc), fd);
    {
      std::unique_lock<std::shared_mutex> lk(sessionsMutex_);
      sessions_.push_back(session);
    }
    if (counters_ != nullptr)
    {
      counters_->subscribers.fetch_add(1, std::memory_order_relaxed);
    }

    readLoop(fd, *session);

    {
      std::unique_lock<std::shared_mutex> lk(sessionsMutex_);
      for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
      {
        if (*it == session)
        {
          sessions_.erase(it);
          break;
        }
      }
    }
    if (counters_ != nullptr)
    {
      counters_->subscribers.fetch_sub(1, std::memory_order_relaxed);
    }
    // The writer thread may be parked in a blocking write on a peer that
    // stopped reading; shut the socket down so it comes back and can be
    // joined. The acceptor still owns the close.
    ::shutdown(fd, SHUT_RDWR);
    session->stop();
  }

  void readLoop(int fd, Session& session)
  {
    std::vector<uint8_t> in;
    uint8_t chunk[4096];
    const auto idleLimit = std::chrono::milliseconds(cfg_.idleTimeoutMs);
    const auto beatEvery = std::chrono::milliseconds(cfg_.heartbeatMs);
    auto lastIn = std::chrono::steady_clock::now();
    auto lastBeat = lastIn;

    while (acceptor_.running())
    {
      const ssize_t r = ::recv(fd, chunk, sizeof chunk, 0);
      if (r > 0)
      {
        lastIn = std::chrono::steady_clock::now();
        in.insert(in.end(), chunk, chunk + r);
        if (!drain(in, session))
        {
          return;
        }
      }
      else if (r == 0)
      {
        return;  // peer closed
      }
      else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      {
        return;
      }

      if (session.dead())
      {
        return;  // slow consumer, or the peer vanished mid-write
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - lastIn > idleLimit)
      {
        if (counters_ != nullptr)
        {
          counters_->idleDisconnects.fetch_add(1, std::memory_order_relaxed);
        }
        return;
      }
      if (now - lastBeat >= beatEvery)
      {
        lastBeat = now;
        session.sendHeartbeat();
      }
    }
  }

  // Decode as many complete messages as the buffer holds. False tears the
  // session down (protocol error, or the peer asked to close).
  bool drain(std::vector<uint8_t>& in, Session& session)
  {
    size_t off = 0;
    while (off < in.size())
    {
      size_t consumed = 0;
      std::vector<MdRequest> reqs;
      std::vector<uint8_t> reply;
      const MdEncoder::Parse st =
          session.parse(in.data() + off, in.size() - off, consumed, reqs, reply);
      if (st == MdEncoder::Parse::Need)
      {
        break;
      }
      if (st == MdEncoder::Parse::Bad || consumed == 0)
      {
        return false;
      }
      off += consumed;
      session.enqueue(reply);
      bool close = false;
      for (const MdRequest& req : reqs)
      {
        if (req.kind == MdRequestKind::Close)
        {
          close = true;
          continue;
        }
        session.handle(req);
      }
      if (close)
      {
        in.erase(in.begin(), in.begin() + static_cast<long>(off));
        return false;
      }
    }
    in.erase(in.begin(), in.begin() + static_cast<long>(off));
    return true;
  }

  MdCounters* counters_;
  MdDistributionConfig cfg_;
  FixMdConfig fixCfg_;
  SocketAcceptor acceptor_;
  mutable std::mutex sourcesMutex_;
  std::unordered_map<SymbolId, Source> sources_;
  std::unordered_map<uint8_t, EncoderFactory> encodings_;
  mutable std::shared_mutex sessionsMutex_;
  std::vector<std::shared_ptr<Session>> sessions_;
};

}  // namespace flox::venue
