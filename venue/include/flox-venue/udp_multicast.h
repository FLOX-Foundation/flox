/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/market_data.h"
#include "flox-venue/metrics.h"
#include "flox-venue/resend_buffer.h"
#include "flox-venue/sbe_md_codec.h"

#include "flox/net/socket.h"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace flox::venue
{

// An interface selector as the callers spell it: nullptr or "" means "let the
// routing table decide", which is what the layer's empty string means too.
inline std::string ifaceOrAny(const char* ifaceIp)
{
  return ifaceIp == nullptr ? std::string{} : std::string{ifaceIp};
}

class UdpMdPublisher
{
 public:
  // ifaceIp selects the egress interface for multicast: nullptr/"" lets the
  // kernel routing table pick (INADDR_ANY) -- the correct production default so
  // the feed reaches co-located clients on the data NIC. Pass "127.0.0.1" to
  // pin loopback (same-host tests). ttl bounds the multicast scope (1 = local
  // segment, the colo default).
  bool open(const char* group, uint16_t port, bool multicast = true, const char* ifaceIp = nullptr,
            unsigned char ttl = 1)
  {
    fd_ = net::openSocket(net::Kind::Udp);
    if (!net::valid(fd_))
    {
      return false;
    }
    // A group that does not parse used to leave the destination at 0.0.0.0 and
    // every publish silently went nowhere. It is refused here instead.
    if (!net::parseAddress(group, dst_, port))
    {
      close();
      return false;
    }
    if (multicast)
    {
      net::setMulticastInterface(fd_, ifaceOrAny(ifaceIp));
      net::setMulticastLoop(fd_, true);
      net::setMulticastTtl(fd_, ttl);
    }
    // Non-blocking: a full socket buffer must never stall the matching thread.
    // The failed datagram is a counted drop, not a wait -- consumers recover
    // the hole via gap detection + the recovery channel.
    net::setNonBlocking(fd_, true);
    return true;
  }

  void setCounters(MdCounters* counters) noexcept { counters_ = counters; }

  // Returns false when the datagram was NOT handed to the kernel (socket
  // buffer full, closed fd, ...). The drop is counted; the publisher never
  // blocks and never retries -- gap recovery is the consumer's job.
  bool publish(const MdMessage& m)
  {
    SbeMdCodec::encode(m, buf_);
    const long n = net::sendTo(fd_, buf_.data(), buf_.size(), dst_);
    if (n != static_cast<long>(buf_.size()))
    {
      if (counters_ != nullptr)
      {
        counters_->sendDrops.fetch_add(1, std::memory_order_relaxed);
      }
      return false;
    }
    return true;
  }

  void close()
  {
    net::closeSocket(fd_);
    fd_ = net::kInvalid;
  }
  ~UdpMdPublisher() { close(); }

 private:
  net::Handle fd_{net::kInvalid};
  ::sockaddr_in dst_{};
  std::vector<uint8_t> buf_;
  MdCounters* counters_{nullptr};
};

class UdpMdSubscriber
{
 public:
  // ifaceIp selects the interface to receive the group on: nullptr/"" ->
  // INADDR_ANY (kernel default). Pass "127.0.0.1" to pin loopback (same-host
  // tests). Mirrors UdpMdPublisher::open.
  bool join(const char* group, uint16_t port, bool multicast = true, const char* ifaceIp = nullptr)
  {
    fd_ = net::openSocket(net::Kind::Udp);
    if (!net::valid(fd_))
    {
      return false;
    }
    net::setReuseAddr(fd_, true);
    // A multicast receiver binds the wildcard so the group's datagrams reach
    // it whatever interface they arrive on; a unicast one binds loopback.
    if (!net::bindTo(fd_, multicast ? "" : "127.0.0.1", port))
    {
      return false;
    }
    port_ = net::boundPort(fd_);
    if (multicast && !net::joinMulticast(fd_, group, ifaceOrAny(ifaceIp)))
    {
      return false;
    }
    return true;
  }

  void setTimeout(int ms) { net::setReceiveTimeout(fd_, ms); }

  // Optional client-side sequencing: with a detector attached, recv() delivers
  // messages strictly in seq order per (symbol, epoch) -- held-out reordered
  // datagrams are buffered and drained when the missing seq arrives -- and
  // gap / publisher-restart signals surface through the callbacks. The
  // detector is owned by the caller and must outlive the subscriber. Without
  // a detector recv() is the raw decode path, unchanged.
  void setGapDetector(GapDetector* gd, GapDetector::GapFn onGap = {},
                      GapDetector::EpochFn onEpoch = {})
  {
    gd_ = gd;
    onGap_ = std::move(onGap);
    onEpoch_ = std::move(onEpoch);
  }

  // Receive and decode one message; returns false on timeout / error. With a
  // gap detector attached, keeps reading until an in-order message is
  // deliverable (held-out datagrams do not surface) or the socket times out.
  bool recv(MdMessage& out)
  {
    if (gd_ == nullptr)
    {
      return recvRaw(out);
    }
    while (ready_.empty())
    {
      MdMessage m;
      if (!recvRaw(m))
      {
        return false;
      }
      gd_->observe(m, [this](const MdMessage& d)
                   { ready_.push_back(d); }, onGap_, onEpoch_);
    }
    out = ready_.front();
    ready_.pop_front();
    return true;
  }

  // After applying a snapshot with lastSeq L: fast-forward the stream to L+1.
  // Held datagrams beyond the snapshot become deliverable immediately.
  void resetSequencer(SymbolId symbol, uint64_t epoch, uint64_t nextSeq)
  {
    if (gd_ != nullptr)
    {
      gd_->reset(symbol, epoch, nextSeq, [this](const MdMessage& d)
                 { ready_.push_back(d); });
    }
  }

  int port() const noexcept { return port_; }

  void close()
  {
    net::closeSocket(fd_);
    fd_ = net::kInvalid;
  }
  ~UdpMdSubscriber() { close(); }

 private:
  bool recvRaw(MdMessage& out)
  {
    uint8_t buf[SbeMdCodec::kMaxSize];
    const long n = net::receiveFrom(fd_, buf, sizeof buf);
    if (n <= 0)
    {
      return false;
    }
    return SbeMdCodec::decode(buf, static_cast<size_t>(n), out);
  }

  net::Handle fd_{net::kInvalid};
  int port_{0};
  GapDetector* gd_{nullptr};
  GapDetector::GapFn onGap_;
  GapDetector::EpochFn onEpoch_;
  std::deque<MdMessage> ready_;  // sequenced, deliverable messages
};

}  // namespace flox::venue
