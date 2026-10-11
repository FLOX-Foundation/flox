/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Time bars start on wall-clock boundaries.
//
// Trade timestamps enter the engine as Unix nanoseconds and are carried as
// LrvxClock (steady_clock) TimePoints through unix_to_lrvx_offset_ns(), a
// per-process distance between the two clocks. The bar policies snapped the
// TimePoint's own count to the interval, so a 60 s bucket started wherever
// the steady clock's epoch put it: ten-odd seconds into the minute on one
// run, a different number of seconds on the next. The same tape then
// aggregated into different bars in different processes.
//
// The existing aggregator tests never anchor the offset (it stays 0), where
// both snapping rules agree. These tests anchor it to a value that is not a
// multiple of the interval and check the bars in Unix terms.

#include "lrvx/aggregator/bar_aggregator.h"
#include "lrvx/aggregator/bus/bar_bus.h"
#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/aggregator/policies/heikin_ashi_bar_policy.h"
#include "lrvx/aggregator/policies/time_bar_policy.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/common.h"
#include "lrvx/strategy/abstract_strategy.h"
#include "lrvx/util/base/time.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using namespace lrvx;

namespace
{

constexpr SymbolId SYMBOL = 7;
constexpr int64_t kNsPerSec = 1'000'000'000;
constexpr int64_t kMinuteNs = 60 * kNsPerSec;

// 2026-09-01T00:00:20Z: twenty seconds into a minute, so the bucket start
// (00:00:00) differs from the first trade's timestamp.
constexpr int64_t kBaseUnixNs = 1'788'220'820LL * kNsPerSec;

// 10.394 s, roughly what a steady clock anchored at boot produces, and not
// a multiple of any interval used here.
constexpr int64_t kOffsetA = 10'394'000'000LL;
constexpr int64_t kOffsetB = 47'001'000'123LL;

class OffsetGuard
{
 public:
  explicit OffsetGuard(int64_t offset)
      : _saved(unix_to_lrvx_offset_ns().load(std::memory_order_relaxed))
  {
    unix_to_lrvx_offset_ns().store(offset, std::memory_order_relaxed);
  }
  ~OffsetGuard() { unix_to_lrvx_offset_ns().store(_saved, std::memory_order_relaxed); }

 private:
  int64_t _saved;
};

TradeEvent makeTrade(double price, int64_t unixNs)
{
  TradeEvent event;
  event.trade.symbol = SYMBOL;
  event.trade.instrument = InstrumentType::Spot;
  event.trade.price = Price::fromDouble(price);
  event.trade.quantity = Quantity::fromDouble(1.0);
  event.trade.isBuy = true;
  event.trade.exchangeTsNs = UnixNanos::fromRaw(unixNs);
  return event;
}

class Collector : public IStrategy
{
 public:
  explicit Collector(std::vector<Bar>& out) : _out(out) {}
  SubscriberId id() const override { return 1; }
  void onBar(const BarEvent& event) override { _out.push_back(event.bar); }

 private:
  std::vector<Bar>& _out;
};

struct UnixBar
{
  int64_t startNs;
  int64_t endNs;
  double open;
  double close;
  bool operator==(const UnixBar&) const = default;
};

template <typename Aggregator, typename Policy>
std::vector<UnixBar> aggregate(int64_t offset, std::chrono::seconds interval)
{
  OffsetGuard guard(offset);
  std::vector<Bar> bars;
  BarBus bus;
  bus.enableDrainOnStop();
  Aggregator aggregator(Policy(interval), &bus);
  auto strat = std::make_unique<Collector>(bars);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  // 00:00:20, 00:00:50 in the first minute; 00:01:05, 00:01:30 in the
  // second; 00:02:10 opens a third bucket and closes the second.
  aggregator.onTrade(makeTrade(100.0, kBaseUnixNs));
  aggregator.onTrade(makeTrade(101.0, kBaseUnixNs + 30 * kNsPerSec));
  aggregator.onTrade(makeTrade(102.0, kBaseUnixNs + 45 * kNsPerSec));
  aggregator.onTrade(makeTrade(103.0, kBaseUnixNs + 70 * kNsPerSec));
  aggregator.onTrade(makeTrade(104.0, kBaseUnixNs + 110 * kNsPerSec));

  bus.stop();

  std::vector<UnixBar> out;
  for (const auto& b : bars)
  {
    out.push_back({lrvxNsToUnixNs(b.startTime.time_since_epoch().count()),
                   lrvxNsToUnixNs(b.endTime.time_since_epoch().count()), b.open.toDouble(),
                   b.close.toDouble()});
  }
  return out;
}

}  // namespace

TEST(TimeBarAlignment, BarsStartOnUnixMinuteWithAnchoredOffset)
{
  const auto bars = aggregate<TimeBarAggregator, TimeBarPolicy>(kOffsetA, std::chrono::seconds(60));
  ASSERT_EQ(bars.size(), 2u);

  const int64_t minute0 = kBaseUnixNs - 20 * kNsPerSec;
  EXPECT_EQ(bars[0].startNs, minute0) << "first bar does not start on the wall-clock minute";
  EXPECT_EQ(bars[0].endNs, minute0 + kMinuteNs);
  EXPECT_EQ(bars[1].startNs, minute0 + kMinuteNs);
  EXPECT_EQ(bars[1].endNs, minute0 + 2 * kMinuteNs);
  for (const auto& b : bars)
  {
    EXPECT_EQ(b.startNs % kMinuteNs, 0) << "bar start is " << (b.startNs % kMinuteNs) / kNsPerSec
                                        << " s past the minute";
  }

  // The trade at 00:00:50 (+30 s) belongs to the first bar and 00:01:05
  // (+45 s) to the second. With the bucket shifted 10.394 s into the
  // minute, 00:01:05 fell into the first bar instead.
  EXPECT_EQ(bars[0].open, 100.0);
  EXPECT_EQ(bars[0].close, 101.0);
  EXPECT_EQ(bars[1].open, 102.0);
  EXPECT_EQ(bars[1].close, 103.0);
}

TEST(TimeBarAlignment, SameTapeSameBarsUnderDifferentOffsets)
{
  const auto a = aggregate<TimeBarAggregator, TimeBarPolicy>(kOffsetA, std::chrono::seconds(60));
  const auto b = aggregate<TimeBarAggregator, TimeBarPolicy>(kOffsetB, std::chrono::seconds(60));
  const auto c = aggregate<TimeBarAggregator, TimeBarPolicy>(0, std::chrono::seconds(60));
  EXPECT_EQ(a, b) << "two processes with different clock offsets aggregated different bars";
  EXPECT_EQ(a, c) << "an anchored process disagrees with an unanchored one";
}

TEST(TimeBarAlignment, FiveSecondBarsAlignToo)
{
  const auto bars = aggregate<TimeBarAggregator, TimeBarPolicy>(kOffsetB, std::chrono::seconds(5));
  ASSERT_FALSE(bars.empty());
  for (const auto& b : bars)
  {
    EXPECT_EQ(b.startNs % (5 * kNsPerSec), 0);
    EXPECT_EQ(b.endNs - b.startNs, 5 * kNsPerSec);
  }
}

TEST(TimeBarAlignment, HeikinAshiBarsStartOnUnixMinuteToo)
{
  const auto bars =
      aggregate<HeikinAshiBarAggregator, HeikinAshiBarPolicy>(kOffsetA, std::chrono::seconds(60));
  ASSERT_EQ(bars.size(), 2u);
  const int64_t minute0 = kBaseUnixNs - 20 * kNsPerSec;
  EXPECT_EQ(bars[0].startNs, minute0);
  EXPECT_EQ(bars[1].startNs, minute0 + kMinuteNs);
  const auto other =
      aggregate<HeikinAshiBarAggregator, HeikinAshiBarPolicy>(kOffsetB, std::chrono::seconds(60));
  EXPECT_EQ(bars, other);
}
