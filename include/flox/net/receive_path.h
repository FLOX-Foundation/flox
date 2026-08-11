/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// Receive-path abstraction: the seam between "bytes arrive on the host" and
// "a decoder sees a datagram". Host bypass technology is not the framework's
// business -- the attachment point is. UDP connectors poll an IReceivePath;
// which backend fills it is a deployment decision:
//
//   - UdpSocketReceivePath (here): non-blocking socket + recvmsg, kernel or
//     NIC receive timestamps when available. Works everywhere; the default.
//   - AfXdpReceivePath (flox/net/af_xdp_receive_path.h, Linux-only): kernel
//     keeps the NIC, an eBPF program redirects to a user-space UMEM ring --
//     the pragmatic middle between the kernel path and full DPDK. Implemented
//     against libxdp (xsk) and validated on a Linux host; needs a capable
//     driver. The interface below is the seam it plugs into (poll semantics,
//     borrowed buffers, explicit rx timestamps); see tools/afxdp_probe.cpp.
//
// Contract: payload spans are BORROWED -- valid only inside the callback.
// The decoder consumes or copies before returning; nothing allocates on the
// per-packet path.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "flox/net/rx_timestamp.h"
#include "flox/util/performance/latency_contour.h"

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace flox::net
{

struct Datagram
{
  std::span<const std::byte> payload;  // borrowed: valid inside the callback only
  int64_t rxTimestampNs{0};            // hop zero of the latency contour
};

class IReceivePath
{
 public:
  virtual ~IReceivePath() = default;

  // Drains up to maxPackets, invoking cb per datagram. Returns the number
  // delivered; 0 = nothing pending. Non-blocking: busy-poll loops and
  // backoff live in the caller, not the backend.
  virtual int poll(const std::function<void(const Datagram&)>& cb, int maxPackets) = 0;

  virtual RxTimestampMode timestampMode() const = 0;
};

#if defined(__linux__) || defined(__APPLE__)

// Default backend: non-blocking UDP socket. Kernel (or NIC, Linux + capable
// hardware) receive timestamps via rx_timestamp.h; falls back to
// monotonicNs() at recvmsg return when the control message is absent.
class UdpSocketReceivePath : public IReceivePath
{
 public:
  struct Config
  {
    std::string bindAddress{"0.0.0.0"};
    uint16_t port{0};
    std::string multicastGroup{};  // empty = plain unicast
    int recvBufferBytes{4 * 1024 * 1024};
    bool wantHardwareTimestamps{true};
    size_t maxDatagramBytes{65536};
  };

  explicit UdpSocketReceivePath(const Config& cfg) : _buffer(cfg.maxDatagramBytes)
  {
    _fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (_fd < 0)
    {
      return;
    }

    const int one = 1;
    ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &cfg.recvBufferBytes,
                 sizeof(cfg.recvBufferBytes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    addr.sin_addr.s_addr = ::inet_addr(cfg.bindAddress.c_str());
    if (::bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
      ::close(_fd);
      _fd = -1;
      return;
    }

    if (!cfg.multicastGroup.empty())
    {
      ip_mreq mreq{};
      mreq.imr_multiaddr.s_addr = ::inet_addr(cfg.multicastGroup.c_str());
      mreq.imr_interface.s_addr = ::inet_addr(cfg.bindAddress.c_str());
      ::setsockopt(_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    }

    ::fcntl(_fd, F_SETFL, ::fcntl(_fd, F_GETFL, 0) | O_NONBLOCK);
    _tsMode = enableRxTimestamps(_fd, cfg.wantHardwareTimestamps);
  }

  ~UdpSocketReceivePath() override
  {
    if (_fd >= 0)
    {
      ::close(_fd);
    }
  }

  UdpSocketReceivePath(const UdpSocketReceivePath&) = delete;
  UdpSocketReceivePath& operator=(const UdpSocketReceivePath&) = delete;

  bool valid() const noexcept { return _fd >= 0; }
  int fd() const noexcept { return _fd; }
  RxTimestampMode timestampMode() const override { return _tsMode; }

  // Local port after bind (useful when cfg.port == 0 let the OS pick).
  uint16_t localPort() const
  {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (_fd < 0 || ::getsockname(_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
    {
      return 0;
    }
    return ntohs(addr.sin_port);
  }

  int poll(const std::function<void(const Datagram&)>& cb, int maxPackets) override
  {
    int delivered = 0;
    while (delivered < maxPackets)
    {
      iovec iov{_buffer.data(), _buffer.size()};
      alignas(cmsghdr) char control[256];
      msghdr msg{};
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = control;
      msg.msg_controllen = sizeof(control);

      const ssize_t n = ::recvmsg(_fd, &msg, 0);
      if (n <= 0)
      {
        break;  // EAGAIN or error: nothing pending
      }

      Datagram d;
      d.payload = {reinterpret_cast<const std::byte*>(_buffer.data()),
                   static_cast<size_t>(n)};
      d.rxTimestampNs = extractRxTimestampNs(msg);
      if (d.rxTimestampNs == 0)
      {
        d.rxTimestampNs = performance::monotonicNs();
      }
      cb(d);
      ++delivered;
    }
    return delivered;
  }

 private:
  int _fd{-1};
  RxTimestampMode _tsMode{RxTimestampMode::NONE};
  std::vector<char> _buffer;
};

#endif  // __linux__ || __APPLE__

}  // namespace flox::net
