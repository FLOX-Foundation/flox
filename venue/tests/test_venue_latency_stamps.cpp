/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Envelope timestamps: the venue can decompose its own latency.
 *
 * Before this, no inbound command or outbound event carried a time of any
 * kind, so fme_submit_latency_ns exported whatever a caller hand-fed into the
 * histogram -- which outside the tests was nothing. The tests that did feed it
 * exercised the histogram as a container and proved nothing about measurement.
 *
 * This one drives real commands through a real SequencedShard and asserts the
 * histograms fill from the envelope stamps alone: no record() call appears
 * anywhere in this file. That is the property that matters, and it is the one
 * a mutation that drops the stamps must break.
 */

#include "flox-venue/matching_book.h"
#include "flox-venue/metrics.h"
#include "flox-venue/sequenced_shard.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>

using namespace flox;
using namespace flox::venue;

namespace
{

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = 1;
  c.tickSize = Price::fromDouble(0.01);
  c.minPrice = Price::fromDouble(1.0);
  c.maxPrice = Price::fromDouble(1000.0);
  return c;
}

NewOrder order(OrderId id, Side s, double px, double qty)
{
  NewOrder o;
  o.id = id;
  o.symbol = 1;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(px);
  o.quantity = Quantity::fromDouble(qty);
  o.accountId = 7;
  return o;
}

struct MetricsPump final : IEngineEventListener
{
  Metrics m;
  std::atomic<uint64_t> seen{0};

  void onEngineEvent(const EngineEventMsg& msg) override
  {
    m.observe(msg);
    seen.fetch_add(1, std::memory_order_release);
  }
};

}  // namespace

TEST(VenueLatencyStamps, HistogramsFillFromTheRealPath)
{
  const std::string dir =
      (std::filesystem::temp_directory_path() / "flox_latency_stamps_test").string();
  std::filesystem::remove_all(dir);

  MetricsPump pump;
  {
    // Heap, not stack: the shard embeds its ingress and outbound rings inline
    // and overflows a default thread stack -- as a SIGSEGV with no gtest
    // output, which is exactly how it presented here.
    auto shardPtr = std::make_unique<SequencedShard<>>(cfg(), dir, MatchingBook{},
                                                       Journal::Sync::Off);
    auto& shard = *shardPtr;
    shard.subscribeOutbound(&pump);
    shard.start();

    const int64_t wire = venueMonoNs();
    for (int i = 0; i < 32; ++i)
    {
      shard.submit(InboundCommand{order(static_cast<OrderId>(100 + i),
                                        (i % 2 == 0) ? Side::BUY : Side::SELL, 100.0 + (i % 5),
                                        1.0)},
                   wire);
    }
    shard.flush();
    for (int spin = 0; spin < 2000 && pump.seen.load(std::memory_order_acquire) < 32; ++spin)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    shard.stop();
  }

  ASSERT_GE(pump.seen.load(), 32u) << "outbound events never arrived";

  // The point of the test: every histogram filled without a single manual
  // record() in this file. Counts, then sanity on the values -- a steady clock
  // cannot produce a zero-width end-to-end percentile over a real submit.
  EXPECT_GT(pump.m.submitLatency.count(), 0u) << "ingress stamp never reached the observer";
  EXPECT_GT(pump.m.wireToIngress.count(), 0u) << "wire stamp was dropped on the way in";
  EXPECT_GT(pump.m.applyToDeliver.count(), 0u) << "publish stamp was dropped on the way out";
  EXPECT_GT(pump.m.submitLatency.percentileNs(0.5), 0u);

  // Provenance is per-command, not global: an unstamped submit must not
  // inherit a stale stamp from the command before it.
  MetricsPump pump2;
  {
    const std::string dir2 = dir + "_bare";
    std::filesystem::remove_all(dir2);
    auto shardPtr = std::make_unique<SequencedShard<>>(cfg(), dir2, MatchingBook{},
                                                       Journal::Sync::Off);
    auto& shard = *shardPtr;
    shard.subscribeOutbound(&pump2);
    shard.start();
    shard.submit(InboundCommand{order(500, Side::BUY, 100.0, 1.0)});  // no wire stamp
    shard.flush();
    for (int spin = 0; spin < 2000 && pump2.seen.load(std::memory_order_acquire) < 1; ++spin)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    shard.stop();
  }
  EXPECT_GT(pump2.m.submitLatency.count(), 0u) << "ingress stamping must not depend on the wire";
  EXPECT_EQ(pump2.m.wireToIngress.count(), 0u)
      << "a submit without a wire stamp fabricated a wire-to-ingress sample";

  std::printf("submit p50=%lluns p99=%lluns  wire->ingress p50=%lluns  apply->deliver p50=%lluns\n",
              (unsigned long long)pump.m.submitLatency.percentileNs(0.5),
              (unsigned long long)pump.m.submitLatency.percentileNs(0.99),
              (unsigned long long)pump.m.wireToIngress.percentileNs(0.5),
              (unsigned long long)pump.m.applyToDeliver.percentileNs(0.5));
}
