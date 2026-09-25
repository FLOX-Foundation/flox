/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Semantics of the two aggregation policies whose close path is driven by
// something other than the trade that happens to arrive: Renko (a brick is a
// price construct, not a container of trades) and Time (a bucket is a clock
// construct, so a trade can arrive for a bucket that is already gone).
//
// Three properties are pinned here:
//
//   1. A Renko brick closes at a brick boundary. The trade that crosses the
//      boundary is applied first, the brick is emitted with open/close exactly
//      one brick apart and in the direction of the move, and the next brick
//      opens at that boundary -- not at the crossing trade's price.
//   2. A single trade cannot emit an unbounded number of bricks. The number of
//      bars published from one onTrade() is bounded by a constant, and the
//      price the aggregator carries forward still tracks the trade.
//   3. A trade whose bucket precedes the live time bar is not folded into that
//      bar. Its price never becomes the bar's close, high or low, and it is not
//      counted in tradeCount. Out-of-order trades that still belong to the live
//      bucket are unaffected.

#include "flox/aggregator/bar_aggregator.h"
#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/aggregator/multi_timeframe_aggregator.h"
#include "flox/aggregator/policies/renko_bar_policy.h"
#include "flox/aggregator/policies/time_bar_policy.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/strategy/abstract_strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <vector>

using namespace flox;

namespace
{

constexpr SymbolId SYMBOL = 7;
const std::chrono::seconds INTERVAL = std::chrono::seconds(60);

// The largest number of bars one trade may publish. A gap of several brick
// widths has to be walked in whole bricks, but the walk is bounded: the cost
// of a single print must not scale with how far the price moved.
//
// needs: the same bound exposed by the implementation, e.g.
//   static constexpr std::size_t RenkoBarPolicy::kMaxGapBricks;
// so this constant can be replaced by the policy's own and the two cannot
// drift apart.
constexpr std::size_t kMaxBarsPerTrade = 1024;

TimePoint ts(int seconds) { return TimePoint(std::chrono::seconds(seconds)); }

TradeEvent makeTrade(double price, double qty, int sec, bool isBuy = true)
{
  TradeEvent event;
  event.trade.symbol = SYMBOL;
  event.trade.instrument = InstrumentType::Spot;
  event.trade.price = Price::fromDouble(price);
  event.trade.quantity = Quantity::fromDouble(qty);
  event.trade.isBuy = isBuy;
  event.trade.exchangeTsNs = UnixNanos::fromRaw(ts(sec).time_since_epoch().count());
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

// Counts instead of storing: the unbounded-gap case publishes six figures of
// bars on the unfixed tree and the point of the test is the count, not the
// contents.
class Counter : public IStrategy
{
 public:
  SubscriberId id() const override { return 2; }

  void onBar(const BarEvent& event) override
  {
    ++count;
    last = event.bar;
  }

  std::size_t count = 0;
  Bar last{};
};

}  // namespace

// ============================================================================
// Renko: where a brick closes, and where the next one opens
// ============================================================================

// Brick size 10, trades 100, 95, 111. The 111 crosses the boundary above 100,
// so the brick that closes runs 100 -> 110: one brick tall, pointing up. The
// brick used to be emitted *before* the crossing trade was applied, so it came
// out as whatever the bar happened to hold -- 100 -> 95, five points tall and
// pointing down, for a move that went up.
TEST(RenkoSemanticsTest, UpBrickClosesAtTheBrickBoundary)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(10.0), &bus);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(100.0, 1.0, 0));
  aggregator.onTrade(makeTrade(95.0, 1.0, 1));
  aggregator.onTrade(makeTrade(111.0, 1.0, 2));

  bus.stop();

  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].open, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].close, Price::fromDouble(110.0));
  EXPECT_GT(result[0].close.raw(), result[0].open.raw()) << "the move was up";
  EXPECT_EQ(result[0].close.raw() - result[0].open.raw(), Price::fromDouble(10.0).raw())
      << "a brick is exactly one brick tall";
  EXPECT_GE(result[0].high.raw(), result[0].close.raw());
  EXPECT_LE(result[0].low.raw(), result[0].open.raw());
}

// The crossing trade completes the brick, so it belongs to the brick it
// completed: its notional and its tick are counted there, not carried into the
// brick that opens next (which no trade has touched yet).
TEST(RenkoSemanticsTest, CrossingTradeIsCountedInTheBrickItCompletes)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(10.0), &bus);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(100.0, 1.0, 0));
  aggregator.onTrade(makeTrade(95.0, 1.0, 1));
  aggregator.onTrade(makeTrade(111.0, 1.0, 2));

  bus.stop();

  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].tradeCount.raw(), 3);
  EXPECT_EQ(result[0].volume, Volume::fromDouble(100.0 + 95.0 + 111.0));
}

// The next brick starts where the last one ended. Trades 100, 95, 111, 121 at
// brick size 10 are two bricks, 100 -> 110 and 110 -> 120. Re-opening at the
// crossing trade's price (111) instead of the boundary shifted the whole
// series off the brick grid: the second brick came out 111 -> 111, zero tall.
TEST(RenkoSemanticsTest, NextBrickOpensAtTheBoundaryNotAtTheTradePrice)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(10.0), &bus);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(100.0, 1.0, 0));
  aggregator.onTrade(makeTrade(95.0, 1.0, 1));
  aggregator.onTrade(makeTrade(111.0, 1.0, 2));
  aggregator.onTrade(makeTrade(121.0, 1.0, 3));

  bus.stop();

  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].open, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].close, Price::fromDouble(110.0));
  EXPECT_EQ(result[1].open, Price::fromDouble(110.0));
  EXPECT_EQ(result[1].close, Price::fromDouble(120.0));
  EXPECT_EQ(result[1].open, result[0].close) << "bricks chain: each opens at the previous close";
}

// The same rule downwards, followed by a reversal. Trades 100, 105, 89, 101 at
// brick size 10: a down brick 100 -> 90, then an up brick 90 -> 100. The
// emit-before-apply order reported the first brick as 100 -> 105, an *up*
// brick for a move that went down 11 points.
TEST(RenkoSemanticsTest, DownBrickClosesAtTheBoundaryAndTheReversalOpensThere)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(10.0), &bus);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(100.0, 1.0, 0));
  aggregator.onTrade(makeTrade(105.0, 1.0, 1));
  aggregator.onTrade(makeTrade(89.0, 1.0, 2));
  aggregator.onTrade(makeTrade(101.0, 1.0, 3));

  bus.stop();

  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].open, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].close, Price::fromDouble(90.0));
  EXPECT_LT(result[0].close.raw(), result[0].open.raw()) << "the move was down";

  EXPECT_EQ(result[1].open, Price::fromDouble(90.0));
  EXPECT_EQ(result[1].close, Price::fromDouble(100.0));
  EXPECT_GT(result[1].close.raw(), result[1].open.raw()) << "reversal: the next brick is up";
}

// A gap leaves the next brick on the grid too. 100 -> 155 at brick size 10
// spans five whole bricks (the last one ending at 150); the 5 points left over
// belong to the brick that stays open, which therefore opens at 150. A further
// trade at 161 is then 11 points from 150 and closes a sixth brick,
// 150 -> 160. Opening at 155 instead swallowed that trade entirely.
TEST(RenkoSemanticsTest, GapLeavesTheNextBrickOpenAtTheBoundary)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(10.0), &bus);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(100.0, 1.0, 0));
  aggregator.onTrade(makeTrade(155.0, 1.0, 1));
  aggregator.onTrade(makeTrade(161.0, 1.0, 2));

  bus.stop();

  ASSERT_EQ(result.size(), 6u) << "5 bricks for the gap, then 150 -> 160";
  EXPECT_EQ(result[0].open, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].close, Price::fromDouble(110.0));
  EXPECT_EQ(result[5].open, Price::fromDouble(150.0));
  EXPECT_EQ(result[5].close, Price::fromDouble(160.0));

  for (std::size_t i = 1; i < result.size(); ++i)
  {
    EXPECT_EQ(result[i].open, result[i - 1].close) << "brick " << i << " must open at the previous close";
    EXPECT_EQ(std::abs(result[i].close.raw() - result[i].open.raw()), Price::fromDouble(10.0).raw())
        << "brick " << i << " must be exactly one brick tall";
  }
}

// Control: trades that stay inside the brick emit nothing, and stop() flushes
// the partial brick as Forced. Unchanged by any of the above.
TEST(RenkoSemanticsTest, TradesInsideOneBrickEmitNothingUntilForced)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(10.0), &bus);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(100.0, 1.0, 0));
  aggregator.onTrade(makeTrade(105.0, 1.0, 1));
  aggregator.onTrade(makeTrade(95.0, 1.0, 2));

  aggregator.stop();
  bus.stop();

  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].open, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].close, Price::fromDouble(95.0));
  EXPECT_EQ(result[0].tradeCount.raw(), 3);
  EXPECT_EQ(result[0].reason, BarCloseReason::Forced);
}

// ============================================================================
// Renko: the gap is bounded
// ============================================================================

// Brick size 0.01 and a print that jumps from 1.00 to 1000.00 spans 99,900
// brick widths. Walking every one of them inside a single onTrade() allocates
// and publishes 99,900 bars -- a 1.00 -> 100000.00 print at the same brick size
// is ten million -- so one bad print stalls the event bus and blows up memory
// on the hot path. The walk must be capped at a constant.
//
// The cap must not lose the price: the aggregator still has to carry forward a
// brick on the grid at the new level, so a later trade at 1000.02 closes
// 1000.00 -> 1000.01 and 1000.01 -> 1000.02 like any other pair of bricks.
TEST(RenkoGapBoundTest, OneTradeCannotPublishAnUnboundedNumberOfBricks)
{
  BarBus bus;
  bus.enableDrainOnStop();
  RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(0.01), &bus);
  Counter strat;
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(1.0, 1.0, 0));

  const std::size_t before = strat.count;
  aggregator.onTrade(makeTrade(1000.0, 1.0, 1));
  bus.stop();

  const std::size_t fromOneTrade = strat.count - before;
  EXPECT_LE(fromOneTrade, kMaxBarsPerTrade)
      << "a 99,900-brick jump published " << fromOneTrade << " bars from a single trade";
}

TEST(RenkoGapBoundTest, TheCappedGapStillCarriesThePriceForward)
{
  BarBus bus;
  bus.enableDrainOnStop();
  RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(0.01), &bus);
  Counter strat;
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(1.0, 1.0, 0));
  aggregator.onTrade(makeTrade(1000.0, 1.0, 1));
  aggregator.onTrade(makeTrade(1000.02, 1.0, 2));
  bus.stop();

  EXPECT_EQ(strat.last.open, Price::fromDouble(1000.01));
  EXPECT_EQ(strat.last.close, Price::fromDouble(1000.02))
      << "after the capped gap the grid must sit at the new price level, not at 1.00";
}

#if defined(FLOX_RENKO_GAP_BOUND_API)
// needs: static constexpr std::size_t RenkoBarPolicy::kMaxGapBricks
// needs: BarCloseReason::Gap (or an equivalent marker on the bar that carries
//        the remainder of a capped gap), so a consumer can tell a walked gap
//        from a truncated one instead of silently seeing fewer bricks than the
//        move spanned.
TEST(RenkoGapBoundTest, TheCapIsDocumentedAndTheRemainderIsMarked)
{
  static_assert(RenkoBarPolicy::kMaxGapBricks <= kMaxBarsPerTrade);

  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(0.01), &bus);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(1.0, 1.0, 0));
  aggregator.onTrade(makeTrade(1000.0, 1.0, 1));
  bus.stop();

  ASSERT_LE(result.size(), RenkoBarPolicy::kMaxGapBricks);
  EXPECT_EQ(result.back().reason, BarCloseReason::Gap)
      << "the bar that absorbs the un-walked remainder must say so";
  EXPECT_EQ(result.back().close, Price::fromDouble(1000.0));
}
#endif

// ============================================================================
// Time bars: a trade from an earlier bucket
// ============================================================================

// The live bar is [60, 120). A trade stamped t=30 belongs to [0, 60), a bucket
// that is not open -- here it never even existed. Folding it in made its price
// the bar's close (and its low), so the bar published for [60, 120) closed at a
// price that was never traded in that minute. Such a trade is dropped.
TEST(TimeBarLateTradeTest, LateTradeDoesNotOverwriteTheLiveBarClose)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(100.0, 1.0, 60));
  aggregator.onTrade(makeTrade(105.0, 1.0, 90));
  aggregator.onTrade(makeTrade(90.0, 1.0, 30));  // late: belongs to [0, 60)
  aggregator.onTrade(makeTrade(106.0, 1.0, 120));

  bus.stop();

  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].startTime, ts(60));
  EXPECT_EQ(result[0].close, Price::fromDouble(105.0));
  EXPECT_EQ(result[0].high, Price::fromDouble(105.0));
  EXPECT_EQ(result[0].low, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].tradeCount.raw(), 2);
  EXPECT_EQ(result[0].volume, Volume::fromDouble(100.0 + 105.0));
}

// Same rule when the bucket the late trade belongs to really did exist and was
// already published: the bar for [0, 60) is closed and gone, and the trade does
// not get a second life inside [60, 120).
TEST(TimeBarLateTradeTest, LateTradeIsNotResurrectedIntoTheNextBar)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(50.0, 1.0, 10));
  aggregator.onTrade(makeTrade(100.0, 1.0, 70));   // closes [0, 60), opens [60, 120)
  aggregator.onTrade(makeTrade(999.0, 1.0, 30));   // late: [0, 60) is already published
  aggregator.onTrade(makeTrade(101.0, 1.0, 130));  // closes [60, 120)

  bus.stop();

  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].startTime, ts(0));
  EXPECT_EQ(result[0].close, Price::fromDouble(50.0));
  EXPECT_EQ(result[0].tradeCount.raw(), 1);

  EXPECT_EQ(result[1].startTime, ts(60));
  EXPECT_EQ(result[1].open, Price::fromDouble(100.0));
  EXPECT_EQ(result[1].high, Price::fromDouble(100.0));
  EXPECT_EQ(result[1].close, Price::fromDouble(100.0));
  EXPECT_EQ(result[1].tradeCount.raw(), 1);
}

// Control, and the limit of the rule above: a trade that arrives out of order
// but still belongs to the live bucket is ordinary data and must be folded in.
// Only a trade from an *earlier* bucket is dropped.
TEST(TimeBarLateTradeTest, OutOfOrderTradeInsideTheLiveBucketIsStillFolded)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(100.0, 1.0, 60));
  aggregator.onTrade(makeTrade(105.0, 1.0, 90));
  aggregator.onTrade(makeTrade(103.0, 1.0, 65));  // out of order, same bucket
  aggregator.onTrade(makeTrade(106.0, 1.0, 120));

  bus.stop();

  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].startTime, ts(60));
  EXPECT_EQ(result[0].close, Price::fromDouble(103.0));
  EXPECT_EQ(result[0].tradeCount.raw(), 3);
  EXPECT_EQ(result[0].volume, Volume::fromDouble(100.0 + 105.0 + 103.0));
}

// The same trade tape through MultiTimeframeAggregator, which runs its own
// copy of the close path (processPolicy) over the same TimeBarPolicy. The late
// trade must be dropped there too -- a fix applied only to BarAggregator leaves
// every multi-timeframe consumer with the overwritten close.
TEST(TimeBarLateTradeTest, MultiTimeframeAggregatorDropsTheLateTradeToo)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addTimeInterval(INTERVAL);
  Collector strat(result);
  bus.subscribe(&strat);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(100.0, 1.0, 60));
  aggregator.onTrade(makeTrade(105.0, 1.0, 90));
  aggregator.onTrade(makeTrade(90.0, 1.0, 30));  // late: belongs to [0, 60)
  aggregator.onTrade(makeTrade(106.0, 1.0, 120));

  bus.stop();

  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].startTime, ts(60));
  EXPECT_EQ(result[0].close, Price::fromDouble(105.0));
  EXPECT_EQ(result[0].low, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].tradeCount.raw(), 2);
}

#if defined(FLOX_LATE_TRADE_COUNTER_API)
// needs: std::uint64_t BarAggregator<Policy>::lateTradeCount() const noexcept
// A dropped trade must be counted, not merely discarded: silently losing feed
// data is exactly what a cross-venue merge produces, and an operator has to be
// able to see how much of it is being thrown away.
TEST(TimeBarLateTradeTest, DroppedTradesAreCounted)
{
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(100.0, 1.0, 60));
  EXPECT_EQ(aggregator.lateTradeCount(), 0u);

  aggregator.onTrade(makeTrade(90.0, 1.0, 30));
  aggregator.onTrade(makeTrade(91.0, 1.0, 10));
  EXPECT_EQ(aggregator.lateTradeCount(), 2u);

  aggregator.onTrade(makeTrade(101.0, 1.0, 65));
  EXPECT_EQ(aggregator.lateTradeCount(), 2u) << "same-bucket trades are not late";

  bus.stop();
}
#endif
