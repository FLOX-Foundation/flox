/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * What the decoder seam costs on the receive path.
 *
 * The subscriber's decoder became a template parameter so a consumer of
 * another venue's feed can keep the transport, the gap detector and the
 * resequencing. The question that has to be answered with a number rather
 * than an argument is whether that cost anything for the venue's own feed.
 *
 * Both arms receive the same datagrams over the same loopback group. One goes
 * through UdpMdSubscriberT<SbeMdCodec>; the other is what the code did before
 * the seam existed -- receive into a buffer, call the codec directly.
 */
#include "flox-venue/udp_multicast.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace flox;
using namespace flox::venue;
using clk = std::chrono::steady_clock;

namespace
{

uint64_t pct(std::vector<uint64_t>& v, double p)
{
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(static_cast<double>(v.size() - 1) * p)];
}

}  // namespace

int main()
{
  constexpr int kIters = 200000;

  // Arm A: through the seam.
  UdpMdSubscriber sub;
  if (!sub.join("239.255.9.9", 0, /*multicast=*/false))
  {
    std::puts("could not bind the subscriber");
    return 1;
  }
  sub.setTimeout(500);
  UdpMdPublisher pubA;
  if (!pubA.open("127.0.0.1", static_cast<uint16_t>(sub.port()), /*multicast=*/false))
  {
    std::puts("could not open the publisher");
    return 1;
  }

  // Arm B: a bare socket, the way recvRaw read before the seam.
  const net::Handle raw = net::openSocket(net::Kind::Udp);
  if (!net::valid(raw) || !net::setReuseAddr(raw, true) || !net::bindTo(raw, "127.0.0.1", 0))
  {
    std::puts("could not bind the raw socket");
    return 1;
  }
  net::setReceiveTimeout(raw, 500);
  UdpMdPublisher pubB;
  if (!pubB.open("127.0.0.1", net::boundPort(raw), /*multicast=*/false))
  {
    std::puts("could not open the second publisher");
    return 1;
  }

  MdMessage m;
  m.type = MdType::AddOrder;
  m.symbol = 1;
  m.epoch = 1;

  std::vector<uint64_t> seam;
  std::vector<uint64_t> direct;
  seam.reserve(kIters);
  direct.reserve(kIters);

  for (int i = 0; i < kIters; ++i)
  {
    m.seq = static_cast<uint64_t>(i + 1);
    pubA.publish(m);
    MdMessage got;
    const auto a0 = clk::now();
    const bool okA = sub.recv(got);
    const auto a1 = clk::now();
    if (okA)
    {
      seam.push_back(
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(a1 - a0).count()));
    }

    pubB.publish(m);
    uint8_t buf[SbeMdCodec::kMaxSize];
    MdMessage gotB;
    const auto b0 = clk::now();
    const long n = net::receiveFrom(raw, buf, sizeof buf);
    const bool okB = n > 0 && SbeMdCodec::decode(buf, static_cast<size_t>(n), gotB);
    const auto b1 = clk::now();
    if (okB)
    {
      direct.push_back(
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count()));
    }
  }

  std::printf("%-44s %8s %8s %8s\n", "", "p50", "p90", "p99");
  std::printf("%-44s %6llu ns %6llu ns %6llu ns\n", "direct (as it was before the seam)",
              static_cast<unsigned long long>(pct(direct, 0.50)),
              static_cast<unsigned long long>(pct(direct, 0.90)),
              static_cast<unsigned long long>(pct(direct, 0.99)));
  std::printf("%-44s %6llu ns %6llu ns %6llu ns\n", "UdpMdSubscriberT<SbeMdCodec>",
              static_cast<unsigned long long>(pct(seam, 0.50)),
              static_cast<unsigned long long>(pct(seam, 0.90)),
              static_cast<unsigned long long>(pct(seam, 0.99)));
  net::closeSocket(raw);
  return 0;
}
