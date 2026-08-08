/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Integration probe and benchmark for the receive-path backends. Not a CI
// test: AF_XDP needs CAP_NET_RAW/CAP_BPF and an interface, so this runs by
// hand on a prepared host (veth pair or a real NIC).
//
//   afxdp_probe recv-xdp <ifname> <port> <expected>
//   afxdp_probe recv-socket <bind-ip> <port> <expected>
//   afxdp_probe send <dst-ip> <port> <count> [payload-bytes]
//
// Senders stamp each payload with CLOCK_MONOTONIC ns (valid across network
// namespaces on one host), receivers report packets, drops, and one-way
// latency percentiles from those stamps.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "flox/net/receive_path.h"
#include "flox/util/performance/latency_histogram.h"

#if defined(__linux__) && FLOX_AF_XDP_ENABLED
#include "flox/net/af_xdp_receive_path.h"
#endif

using flox::net::Datagram;
using flox::performance::LatencyHistogram;
using flox::performance::monotonicNs;

namespace
{

struct RecvStats
{
  LatencyHistogram oneWay;
  long received{0};
};

// One-way latency uses a monotonic dequeue stamp taken here, matching the
// sender's CLOCK_MONOTONIC payload stamp. d.rxTimestampNs is deliberately not
// used for this: on the socket backend it is a CLOCK_REALTIME kernel stamp,
// and mixing it with the monotonic send stamp yields nonsense. The kernel
// stamp is what the production latency contour consumes; this probe measures
// send-to-userspace-dequeue on one clock.
void account(RecvStats& st, const Datagram& d, int64_t dequeueNs)
{
  ++st.received;
  if (d.payload.size() >= sizeof(int64_t))
  {
    int64_t sentNs = 0;
    std::memcpy(&sentNs, d.payload.data(), sizeof(sentNs));
    const int64_t delta = dequeueNs - sentNs;
    if (delta > 0)
    {
      st.oneWay.record(delta);
    }
  }
}

int report(const RecvStats& st, long expected)
{
  std::printf("received=%ld expected=%ld\n", st.received, expected);
  std::printf("one-way ns: %s\n", st.oneWay.summary().c_str());
  return st.received == expected ? 0 : 1;
}

template <typename Path>
int runReceiver(Path& path, long expected)
{
  RecvStats st;
  const int64_t deadline = monotonicNs() + int64_t(30) * 1'000'000'000;
  while (st.received < expected && monotonicNs() < deadline)
  {
    path.poll([&](const Datagram& d)
              { account(st, d, monotonicNs()); }, 64);
  }
  return report(st, expected);
}

int runSender(const char* ip, uint16_t port, long count, size_t bytes)
{
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = ::inet_addr(ip);

  std::string payload(bytes < sizeof(int64_t) ? sizeof(int64_t) : bytes, 'x');
  long sent = 0;
  for (long i = 0; i < count; ++i)
  {
    const int64_t now = monotonicNs();
    std::memcpy(payload.data(), &now, sizeof(now));
    if (::sendto(fd, payload.data(), payload.size(), 0,
                 reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) > 0)
    {
      ++sent;
    }
    // Light pacing so the veth queue and the fill ring keep up.
    if ((i & 63) == 0)
    {
      ::usleep(50);
    }
  }
  ::close(fd);
  std::printf("sent=%ld\n", sent);
  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc < 4)
  {
    std::fprintf(stderr, "usage: %s recv-xdp|recv-socket|send ... (see header)\n",
                 argv[0]);
    return 2;
  }
  const std::string mode = argv[1];

  if (mode == "send")
  {
    if (argc < 5)
    {
      std::fprintf(stderr, "usage: %s send <dst-ip> <port> <count> [payload-bytes]\n",
                   argv[0]);
      return 2;
    }
    const auto port = uint16_t(std::atoi(argv[3]));
    const long count = std::atol(argv[4]);
    const size_t bytes = argc > 5 ? size_t(std::atol(argv[5])) : 64;
    return runSender(argv[2], port, count, bytes);
  }

  if (mode == "recv-socket")
  {
    flox::net::UdpSocketReceivePath::Config cfg;
    cfg.bindAddress = argv[2];
    cfg.port = uint16_t(std::atoi(argv[3]));
    flox::net::UdpSocketReceivePath path(cfg);
    if (!path.valid())
    {
      std::fprintf(stderr, "socket path init failed\n");
      return 2;
    }
    return runReceiver(path, std::atol(argv[4]));
  }

#if defined(__linux__) && FLOX_AF_XDP_ENABLED
  if (mode == "recv-xdp")
  {
    flox::net::AfXdpReceivePath::Config cfg;
    cfg.ifname = argv[2];
    cfg.udpPortFilter = uint16_t(std::atoi(argv[3]));
    cfg.skbMode = !(argc > 5 && std::strcmp(argv[5], "native") == 0);
    flox::net::AfXdpReceivePath path(cfg);
    if (!path.valid())
    {
      std::fprintf(stderr, "af_xdp init failed (need CAP_NET_RAW/CAP_BPF?)\n");
      return 2;
    }
    const int rc = runReceiver(path, std::atol(argv[4]));
    std::printf("dropped-non-udp=%llu\n",
                (unsigned long long)path.droppedNonUdp());
    return rc;
  }
#endif

  std::fprintf(stderr, "unknown or unsupported mode: %s\n", mode.c_str());
  return 2;
}
