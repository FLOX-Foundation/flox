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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

namespace flox::venue
{

// Resolve a multicast interface selector to an in_addr value (network order).
// nullptr/"" -> INADDR_ANY (kernel routing picks the egress NIC).
inline uint32_t ifaceAddr(const char* ifaceIp)
{
  if (ifaceIp == nullptr || ifaceIp[0] == '\0')
  {
    return htonl(INADDR_ANY);
  }
  in_addr a{};
  if (::inet_pton(AF_INET, ifaceIp, &a) == 1)
  {
    return a.s_addr;
  }
  return htonl(INADDR_ANY);
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
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0)
    {
      return false;
    }
    std::memset(&dst_, 0, sizeof dst_);
    dst_.sin_family = AF_INET;
    dst_.sin_port = htons(port);
    ::inet_pton(AF_INET, group, &dst_.sin_addr);
    if (multicast)
    {
      in_addr iface{};
      iface.s_addr = ifaceAddr(ifaceIp);
      ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof iface);
      unsigned char loop = 1;
      ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
      ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    }
    // Non-blocking: a full socket buffer must never stall the matching thread.
    // The failed datagram is a counted drop, not a wait -- consumers recover
    // the hole via gap detection + the recovery channel.
    const int fl = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
    return true;
  }

  void setCounters(MdCounters* counters) noexcept { counters_ = counters; }

  // Returns false when the datagram was NOT handed to the kernel (socket
  // buffer full, closed fd, ...). The drop is counted; the publisher never
  // blocks and never retries -- gap recovery is the consumer's job.
  bool publish(const MdMessage& m)
  {
    SbeMdCodec::encode(m, buf_);
    const ssize_t n =
        ::sendto(fd_, buf_.data(), buf_.size(), 0, reinterpret_cast<sockaddr*>(&dst_), sizeof dst_);
    if (n != static_cast<ssize_t>(buf_.size()))
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
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_ = -1;
    }
  }
  ~UdpMdPublisher() { close(); }

 private:
  int fd_{-1};
  sockaddr_in dst_{};
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
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0)
    {
      return false;
    }
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = multicast ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0)
    {
      return false;
    }
    socklen_t sl = sizeof a;
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &sl);
    port_ = ntohs(a.sin_port);
    if (multicast)
    {
      ip_mreq mr{};
      ::inet_pton(AF_INET, group, &mr.imr_multiaddr);
      mr.imr_interface.s_addr = ifaceAddr(ifaceIp);
      if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) < 0)
      {
        return false;
      }
    }
    return true;
  }

  void setTimeout(int ms)
  {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }

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
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_ = -1;
    }
  }
  ~UdpMdSubscriber() { close(); }

 private:
  bool recvRaw(MdMessage& out)
  {
    uint8_t buf[SbeMdCodec::kMaxSize];
    const ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, nullptr, nullptr);
    if (n <= 0)
    {
      return false;
    }
    return SbeMdCodec::decode(buf, static_cast<size_t>(n), out);
  }

  int fd_{-1};
  int port_{0};
  GapDetector* gd_{nullptr};
  GapDetector::GapFn onGap_;
  GapDetector::EpochFn onEpoch_;
  std::deque<MdMessage> ready_;  // sequenced, deliverable messages
};

}  // namespace flox::venue
