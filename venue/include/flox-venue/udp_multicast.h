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
#include "flox-venue/sbe_md_codec.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
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
    return true;
  }

  void publish(const MdMessage& m)
  {
    SbeMdCodec::encode(m, buf_);
    ::sendto(fd_, buf_.data(), buf_.size(), 0, reinterpret_cast<sockaddr*>(&dst_), sizeof dst_);
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

  // Receive and decode one message; returns false on timeout / error.
  bool recv(MdMessage& out)
  {
    uint8_t buf[SbeMdCodec::kMaxSize];
    const ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, nullptr, nullptr);
    if (n <= 0)
    {
      return false;
    }
    return SbeMdCodec::decode(buf, static_cast<size_t>(n), out);
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
  int fd_{-1};
  int port_{0};
};

}  // namespace flox::venue
