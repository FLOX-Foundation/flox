/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>

#if defined(__linux__)
#include <linux/net_tstamp.h>
#include <sys/socket.h>
#include <time.h>
#elif defined(__APPLE__)
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace flox::net
{

// Receive-side timestamping for the latency contour: the receive timestamp
// is hop zero of every tick-to-trade measurement. Kernel timestamps remove
// scheduler wakeup jitter from the measurement; hardware (NIC) timestamps
// additionally remove the driver/IRQ path. Falls back cleanly:
// Linux SO_TIMESTAMPING (hw when the NIC supports it, else software) ->
// macOS SO_TIMESTAMP (microsecond software) -> caller uses monotonicNs().

enum class RxTimestampMode : uint8_t
{
  NONE,      // not enabled; use monotonicNs() at recv return
  SOFTWARE,  // kernel software timestamp
  HARDWARE   // NIC hardware timestamp (Linux + capable NIC only)
};

// Enables receive timestamping on a socket. Returns the mode actually
// enabled. Hardware mode additionally requires the interface to be
// configured (ethtool -T / SIOCSHWTSTAMP), which is host setup, not ours.
inline RxTimestampMode enableRxTimestamps(int fd, bool wantHardware = true)
{
#if defined(__linux__)
  // Hardware and software bits are requested together: accepting the option
  // does not mean the NIC is configured for hardware stamps (that needs
  // SIOCSHWTSTAMP / ethtool -T on the interface), so packets must always
  // carry the software stamp as a per-packet fallback. extractRxTimestampNs
  // prefers the hardware stamp when a packet actually has one. HARDWARE
  // here means "requested and accepted", not "guaranteed on every packet".
  if (wantHardware)
  {
    const unsigned int hwFlags = SOF_TIMESTAMPING_RX_HARDWARE |
                                 SOF_TIMESTAMPING_RAW_HARDWARE |
                                 SOF_TIMESTAMPING_RX_SOFTWARE |
                                 SOF_TIMESTAMPING_SOFTWARE;
    if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &hwFlags, sizeof(hwFlags)) == 0)
    {
      return RxTimestampMode::HARDWARE;
    }
  }
  const unsigned int swFlags =
      SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
  if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &swFlags, sizeof(swFlags)) == 0)
  {
    return RxTimestampMode::SOFTWARE;
  }
  return RxTimestampMode::NONE;
#elif defined(__APPLE__)
  (void)wantHardware;
  const int on = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof(on)) == 0)
  {
    return RxTimestampMode::SOFTWARE;
  }
  return RxTimestampMode::NONE;
#else
  (void)fd;
  (void)wantHardware;
  return RxTimestampMode::NONE;
#endif
}

// Extracts the receive timestamp (ns since epoch of the respective clock)
// from a recvmsg() control buffer. Returns 0 when absent -- caller falls
// back to monotonicNs() taken at recv return.
inline int64_t extractRxTimestampNs(const msghdr& msg)
{
#if defined(__linux__)
  for (cmsghdr* c = CMSG_FIRSTHDR(const_cast<msghdr*>(&msg)); c != nullptr;
       c = CMSG_NXTHDR(const_cast<msghdr*>(&msg), c))
  {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING)
    {
      // scm_timestamping: [0] software, [2] raw hardware.
      const auto* ts = reinterpret_cast<const timespec*>(CMSG_DATA(c));
      const timespec& hw = ts[2];
      const timespec& sw = ts[0];
      const timespec& pick = (hw.tv_sec != 0 || hw.tv_nsec != 0) ? hw : sw;
      return int64_t(pick.tv_sec) * 1'000'000'000 + pick.tv_nsec;
    }
  }
  return 0;
#elif defined(__APPLE__)
  for (cmsghdr* c = CMSG_FIRSTHDR(const_cast<msghdr*>(&msg)); c != nullptr;
       c = CMSG_NXTHDR(const_cast<msghdr*>(&msg), c))
  {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP)
    {
      const auto* tv = reinterpret_cast<const timeval*>(CMSG_DATA(c));
      return int64_t(tv->tv_sec) * 1'000'000'000 + int64_t(tv->tv_usec) * 1'000;
    }
  }
  return 0;
#else
  (void)msg;
  return 0;
#endif
}

}  // namespace flox::net
