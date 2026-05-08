/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Locks in the documented bar-close ordering rule on tied timestamps:
// `MultiTimeframeAggregator` dispatches bars in registration order.
// Strategies should register coarsest-first to receive coarse context
// before fine-grained close events on the same wall-clock instant.
//
// See docs/explanation/bar-close-ordering.md.

#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/multi_timeframe_aggregator.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/abstract_market_data_subscriber.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace
{

using namespace flox;
using namespace std::chrono_literals;

class OrderRecorder : public IMarketDataSubscriber
{
 public:
  std::vector<uint64_t> intervals;
  SubscriberId id() const override { return 1; }
  void onBar(const BarEvent& ev) override { intervals.push_back(ev.barTypeParam); }
};

TradeEvent makeTrade(SymbolId sym, double price, int64_t ts_ns)
{
  TradeEvent ev{};
  ev.trade.symbol = sym;
  ev.trade.price = Price::fromDouble(price);
  ev.trade.quantity = Quantity::fromDouble(0.1);
  ev.trade.exchangeTsNs = UnixNanos{ts_ns};
  ev.trade.isBuy = true;
  ev.trade.instrument = InstrumentType::Spot;
  return ev;
}

}  // namespace

TEST(BarCloseOrdering, RegistrationOrderIsDispatchOrderOnTie)
{
  BarBus bus;
  bus.enableDrainOnStop();
  OrderRecorder rec;
  bus.subscribe(&rec);

  // Three time-bar timeframes; register coarsest-first so the
  // documented rule produces the expected dispatch on tied closes.
  const uint64_t M5_NS = 5ull * 60 * 1'000'000'000ull;
  const uint64_t H1_NS = 60ull * 60 * 1'000'000'000ull;
  const uint64_t H4_NS = 4ull * 60 * 60 * 1'000'000'000ull;

  MultiTimeframeAggregator<4> agg(&bus);
  agg.addTimeInterval(4h);
  agg.addTimeInterval(1h);
  agg.addTimeInterval(5min);

  bus.start();
  agg.start();

  // Drive a tape that crosses an H4 boundary so all three timeframes
  // close on the same trade timestamp. Warm with one trade just after
  // midnight, then a second trade at the next H4 boundary.
  agg.onTrade(makeTrade(1, 100.0, 0));
  agg.onTrade(makeTrade(1, 101.0, static_cast<int64_t>(H4_NS)));
  agg.stop();
  bus.stop();

  // Each aggregator emits at least one bar at the tied close. The
  // first three entries correspond to that close in registration
  // order: H4, H1, M5.
  ASSERT_GE(rec.intervals.size(), 3u);
  EXPECT_EQ(rec.intervals[0], H4_NS) << "coarsest (H4) should fire first";
  EXPECT_EQ(rec.intervals[1], H1_NS) << "H1 should fire second";
  EXPECT_EQ(rec.intervals[2], M5_NS) << "M5 should fire third";
}

TEST(BarCloseOrdering, ReverseRegistrationOrderProducesReverseDispatch)
{
  BarBus bus;
  bus.enableDrainOnStop();
  OrderRecorder rec;
  bus.subscribe(&rec);

  const uint64_t M5_NS = 5ull * 60 * 1'000'000'000ull;
  const uint64_t H1_NS = 60ull * 60 * 1'000'000'000ull;
  const uint64_t H4_NS = 4ull * 60 * 60 * 1'000'000'000ull;

  MultiTimeframeAggregator<4> agg(&bus);
  // Reverse: register fine-first. Documented rule says fine fires
  // first; the test pins this so an accidental sort introduced later
  // will surface.
  agg.addTimeInterval(5min);
  agg.addTimeInterval(1h);
  agg.addTimeInterval(4h);

  bus.start();
  agg.start();
  agg.onTrade(makeTrade(1, 100.0, 0));
  agg.onTrade(makeTrade(1, 101.0, static_cast<int64_t>(H4_NS)));
  agg.stop();
  bus.stop();

  ASSERT_GE(rec.intervals.size(), 3u);
  EXPECT_EQ(rec.intervals[0], M5_NS);
  EXPECT_EQ(rec.intervals[1], H1_NS);
  EXPECT_EQ(rec.intervals[2], H4_NS);
}
