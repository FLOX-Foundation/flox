/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Fill realism for bar-driven and book-driven backtests. Every test here pins
// a rule the simulator has to obey for a backtest number to mean anything:
//
//   1. an order submitted from a bar callback cannot trade at a price the
//      strategy could only know because the bar had already closed;
//   2. a market order consumes the visible ladder instead of printing its
//      whole size at the touch, and what it ate is gone for the next order;
//   3. a triggered stop / take-profit cannot print at the bar extreme when
//      that extreme is in the order's favour;
//   4. a resting limit still fills on bar data when a queue model is
//      configured;
//   5. a second run() on the same runner reports that run, not the sum of
//      every run so far.
//
// The reference bar throughout is open 100, high 110, low 90, close 105.
// Each test names both the price the fill must get and the price it must not.

#include "flox/aggregator/events/bar_event.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/liquidation_engine.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/backtest/venue_stack.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/engine/symbol_registry.h"
#include "flox/execution/rate_limit_policy.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/strategy/strategy.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace flox;

namespace
{

constexpr int64_t kMinuteNs = 60'000'000'000LL;
constexpr SymbolId kSym = 1;

SymbolId addSymbol(SymbolRegistry& reg, const std::string& spelling)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = spelling;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

BarEvent makeBar(SymbolId sym, double open, double high, double low, double close,
                 int64_t startNs)
{
  BarEvent ev{};
  ev.symbol = sym;
  ev.barType = BarType::Time;
  ev.barTypeParam = static_cast<uint64_t>(kMinuteNs);
  ev.bar.open = Price::fromDouble(open);
  ev.bar.high = Price::fromDouble(high);
  ev.bar.low = Price::fromDouble(low);
  ev.bar.close = Price::fromDouble(close);
  ev.bar.volume = Volume::fromDouble(1000.0);
  ev.bar.startTime = TimePoint{std::chrono::nanoseconds{startNs}};
  ev.bar.endTime = TimePoint{std::chrono::nanoseconds{startNs + kMinuteNs}};
  ev.bar.reason = BarCloseReason::Threshold;
  return ev;
}

// Bar 0 is the reference bar: open 100, high 110, low 90, close 105.
// Bar 1 opens at 106 so "the next bar's open" is a price that appears
// nowhere in bar 0. Bar 2 opens at 111 for the same reason.
std::vector<BarEvent> referenceBars(SymbolId sym)
{
  return {
      makeBar(sym, 100.0, 110.0, 90.0, 105.0, 0),
      makeBar(sym, 106.0, 108.0, 104.0, 107.0, kMinuteNs),
      makeBar(sym, 111.0, 112.0, 109.0, 111.5, 2 * kMinuteNs),
  };
}

// Emits one market buy from the bar callback of bar `fireOn`, nothing else.
class OneShotBuyStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  SymbolId target{0};
  size_t fireOn{0};
  size_t bars{0};

  void rearm() { bars = 0; }

 protected:
  void onSymbolBar(SymbolContext& /*ctx*/, const BarEvent& /*ev*/) override
  {
    if (bars == fireOn)
    {
      emitMarketBuy(target, Quantity::fromDouble(1.0));
    }
    ++bars;
  }
};

void pushLadder(SimulatedExecutor& exec, SymbolId sym,
                const std::vector<std::pair<double, double>>& bids,
                const std::vector<std::pair<double, double>>& asks)
{
  std::pmr::monotonic_buffer_resource pool(1024);
  std::pmr::vector<BookLevel> b(&pool);
  std::pmr::vector<BookLevel> a(&pool);
  for (const auto& [px, qty] : bids)
  {
    b.emplace_back(Price::fromDouble(px), Quantity::fromDouble(qty));
  }
  for (const auto& [px, qty] : asks)
  {
    a.emplace_back(Price::fromDouble(px), Quantity::fromDouble(qty));
  }
  exec.onBookUpdate(sym, b, a);
}

Order marketOrder(OrderId id, SymbolId sym, Side side, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

Order limitOrder(OrderId id, SymbolId sym, Side side, double price, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

Order conditionalOrder(OrderId id, SymbolId sym, Side side, OrderType type,
                       double trigger, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = type;
  o.triggerPrice = Price::fromDouble(trigger);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

// Volume-weighted price over every fill of one order. An order that walks the
// ladder may be reported as one averaged fill or as one fill per level; both
// are acceptable, the average is what the strategy actually pays.
double vwap(const std::vector<Fill>& fills, OrderId id)
{
  double notional = 0.0;
  double qty = 0.0;
  for (const auto& f : fills)
  {
    if (f.orderId != id)
    {
      continue;
    }
    notional += f.price.toDouble() * f.quantity.toDouble();
    qty += f.quantity.toDouble();
  }
  return qty > 0.0 ? notional / qty : 0.0;
}

double filledQty(const std::vector<Fill>& fills, OrderId id)
{
  double qty = 0.0;
  for (const auto& f : fills)
  {
    if (f.orderId == id)
    {
      qty += f.quantity.toDouble();
    }
  }
  return qty;
}

}  // namespace

// ===========================================================================
// 1. Look-ahead. runBars() walks the whole bar through the simulator before
//    calling the strategy, so an order the callback emits is matched against
//    prices the strategy could only see because the bar had already closed.
//
// needs: SimulatedExecutor::onBar(SymbolId, Price open, Price high, Price low,
//        Price close) -- the runner cannot hold an order back to the next
//        bar's open while the executor is never told what that open is. The
//        tests below only read fills, so they hold against any spelling that
//        gets the open into the matching pass.
// ===========================================================================

// The strategy is shown bar 0 (open 100, high 110, low 90, close 105) and buys
// from that callback. The earliest price it can trade at is bar 1's open, 106.
TEST(BacktestFillRealism, CallbackOrderDoesNotFillOnTheBarThatTriggeredIt)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  OneShotBuyStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;
  strat.fireOn = 0;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runBars(referenceBars(sym));

  const auto& fills = runner.executor().fills();
  ASSERT_EQ(fills.size(), 1u);
  const double px = fills[0].price.toDouble();
  EXPECT_NE(px, 105.0) << "filled at the close of the bar the callback was shown";
  EXPECT_NE(px, 110.0) << "filled at the high of a bar that had already closed";
  EXPECT_NE(px, 90.0) << "filled at the low of a bar that had already closed";
  EXPECT_NE(px, 100.0) << "filled at the open of a bar that had already closed";
  EXPECT_DOUBLE_EQ(px, 106.0) << "a callback order fills at the next bar's open";
}

// Deferring by one bar is not the same as skipping to the next bar's extremes:
// bar 1 is open 106, high 108, low 104, close 107, and a buy that arrived at
// its open can only pay 106.
TEST(BacktestFillRealism, CallbackOrderDoesNotReachIntoTheNextBarsRange)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  OneShotBuyStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;
  strat.fireOn = 0;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runBars(referenceBars(sym));

  ASSERT_EQ(runner.executor().fills().size(), 1u);
  const double px = runner.executor().fills()[0].price.toDouble();
  EXPECT_NE(px, 104.0) << "filled at the next bar's low, which is also future data";
  EXPECT_NE(px, 108.0) << "filled at the next bar's high, which is also future data";
  EXPECT_DOUBLE_EQ(px, 106.0);
}

// The deferral has to hold on the last bar too: there is no next open, so the
// order stays unfilled rather than printing at the close the callback saw.
TEST(BacktestFillRealism, CallbackOrderOnTheLastBarDoesNotFill)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  OneShotBuyStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;
  strat.fireOn = 2;  // the last bar of referenceBars()

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runBars(referenceBars(sym));

  EXPECT_EQ(runner.executor().fills().size(), 0u)
      << "the final bar's callback order filled at 111.5, the close it was shown";
}

// Green control for area 1. Deferring the fill must not lose the order, split
// it, or change its side or size: one market buy of 1.0 still produces exactly
// one buy fill of 1.0.
TEST(BacktestFillRealism, CallbackOrderStillFillsExactlyOnce)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  OneShotBuyStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;
  strat.fireOn = 0;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  const BacktestResult res = runner.runBars(referenceBars(sym));

  ASSERT_EQ(runner.executor().fills().size(), 1u);
  EXPECT_EQ(runner.executor().fills()[0].side, Side::BUY);
  EXPECT_DOUBLE_EQ(runner.executor().fills()[0].quantity.toDouble(), 1.0);
  EXPECT_EQ(res.fills().size(), 1u);
}

// ===========================================================================
// 2. Depth. tryFillOrder() prints the whole remaining size at the touch and
//    never decrements the book, so size is free and the next order sees an
//    untouched ladder.
// ===========================================================================

// Asks 101 x 1, 102 x 2, 103 x 5. A market buy of 5 takes 1 + 2 + 2 and pays
// (101 + 204 + 206) / 5 = 102.2, not the 101 touch.
TEST(BacktestFillRealism, MarketBuyWalksTheAskLadder)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 1.0}, {102.0, 2.0}, {103.0, 5.0}});
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 5.0));

  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 1), 5.0);
  EXPECT_NEAR(vwap(exec.fills(), 1), 102.2, 1e-9)
      << "a 5-lot buy printed at the 1-lot touch";
}

// Bids 99 x 1, 98 x 2, 97 x 5. A market sell of 5 gets
// (99 + 196 + 194) / 5 = 97.8, not the 99 touch.
TEST(BacktestFillRealism, MarketSellWalksTheBidLadder)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 1.0}, {98.0, 2.0}, {97.0, 5.0}}, {{101.0, 10.0}});
  exec.submitOrder(marketOrder(1, kSym, Side::SELL, 5.0));

  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 1), 5.0);
  EXPECT_NEAR(vwap(exec.fills(), 1), 97.8, 1e-9)
      << "a 5-lot sell printed at the 1-lot touch";
}

// What the first order ate is gone for the second one in the same step: after
// a buy of 5 the ladder holds only 3 @ 103, so the next buy of 1 pays 103.
TEST(BacktestFillRealism, ConsumedDepthIsGoneForTheNextOrder)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 1.0}, {102.0, 2.0}, {103.0, 5.0}});
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 5.0));
  exec.submitOrder(marketOrder(2, kSym, Side::BUY, 1.0));

  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 2), 1.0);
  EXPECT_NEAR(vwap(exec.fills(), 2), 103.0, 1e-9)
      << "the second order was handed the depth the first one had already taken";
}

// Size past the visible ladder must not come back cheaper than the worst level
// it consumed. Asks 101 x 1, 102 x 1; a buy of 10 either fills partially or
// fills through, but its average can never be better than 102.
TEST(BacktestFillRealism, SizePastTheLadderIsNotFilledAtTheTouch)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 1.0}, {102.0, 1.0}});
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 10.0));

  const double qty = filledQty(exec.fills(), 1);
  EXPECT_LE(qty, 10.0);
  if (qty > 0.0)
  {
    EXPECT_GE(vwap(exec.fills(), 1), 102.0)
        << "10 lots printed against a 2-lot ladder at " << vwap(exec.fills(), 1);
  }
}

// Green control for area 2. An order that fits inside the top level still pays
// exactly the touch, and a fresh snapshot restores the ladder: depth is
// consumed within a step, not permanently.
TEST(BacktestFillRealism, TopLevelSizePaysTheTouchAndDepthRefreshes)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 3.0));
  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 1), 3.0);
  EXPECT_NEAR(vwap(exec.fills(), 1), 101.0, 1e-9);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(marketOrder(2, kSym, Side::BUY, 3.0));
  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 2), 3.0);
  EXPECT_NEAR(vwap(exec.fills(), 2), 101.0, 1e-9)
      << "a new book snapshot must restore the ladder";
}

// The same rule seen from the liquidation path. A venue liquidation is a
// market order through the matching engine, so closing 10 lots against bids
// 80 x 1, 79 x 2, 78 x 20 realizes (80 + 158 + 546) / 10 = 78.4, not the
// 80 touch.
TEST(BacktestFillRealism, RoutedLiquidationWalksTheBook)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  LiquidationEngine engine;
  engine.addTier(0.0, 0.005);
  engine.setExecutor(&exec);

  pushLadder(exec, kSym, {{80.0, 1.0}, {79.0, 2.0}, {78.0, 20.0}}, {{81.0, 100.0}});

  // 10 lots long from 100 with equity 10: underwater at a mark of 80.
  engine.openPosition(LeveragedPosition{.accountId = 1,
                                        .symbol = kSym,
                                        .quantity = 10.0,
                                        .entryPrice = 100.0,
                                        .equity = 10.0});
  const auto out = engine.onMark(kSym, 80.0);
  ASSERT_EQ(out.liquidationsCount, 1u);

  ASSERT_FALSE(exec.fills().empty());
  const OrderId liqId = exec.fills().front().orderId;
  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), liqId), 10.0);
  EXPECT_NEAR(vwap(exec.fills(), liqId), 78.4, 1e-9)
      << "the liquidation printed its whole size at the touch";
}

// ===========================================================================
// 3. Conditional orders. onBar() walks low -> high -> close, and a conditional
//    that triggers on a step is matched while bid = ask = that step's price,
//    so it books the extreme instead of the price that armed it.
// ===========================================================================

// Bar 100 / 110 / 90 / 105. A sell take-profit armed at 105 books 105; the
// 110 high is a price the order had no claim on.
TEST(BacktestFillRealism, TakeProfitSellFillsAtItsTriggerNotTheBarHigh)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  exec.submitOrder(
      conditionalOrder(1, kSym, Side::SELL, OrderType::TAKE_PROFIT_MARKET, 105.0, 1.0));
  exec.onBar(kSym, Price::fromDouble(110.0), Price::fromDouble(90.0),
             Price::fromDouble(105.0));

  ASSERT_EQ(exec.fills().size(), 1u);
  const double px = exec.fills()[0].price.toDouble();
  EXPECT_NE(px, 110.0) << "the take-profit was handed the bar's high";
  EXPECT_DOUBLE_EQ(px, 105.0);
}

// Same bar, mirrored: a buy take-profit armed at 95 books 95, not the 90 low.
TEST(BacktestFillRealism, TakeProfitBuyFillsAtItsTriggerNotTheBarLow)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  exec.submitOrder(
      conditionalOrder(1, kSym, Side::BUY, OrderType::TAKE_PROFIT_MARKET, 95.0, 1.0));
  exec.onBar(kSym, Price::fromDouble(110.0), Price::fromDouble(90.0),
             Price::fromDouble(105.0));

  ASSERT_EQ(exec.fills().size(), 1u);
  const double px = exec.fills()[0].price.toDouble();
  EXPECT_NE(px, 90.0) << "the take-profit was handed the bar's low";
  EXPECT_DOUBLE_EQ(px, 95.0);
}

// Green control for area 3, first half: a stop-limit already refuses the bar
// high because the limit caps it. Armed at 105 with a limit of 104, it books
// 104 today and must keep booking no better than its own trigger.
TEST(BacktestFillRealism, TriggeredStopLimitDoesNotPrintTheBarHigh)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  Order o = conditionalOrder(1, kSym, Side::SELL, OrderType::STOP_LIMIT, 105.0, 1.0);
  o.price = Price::fromDouble(104.0);
  o.triggerPrice = Price::fromDouble(105.0);
  // A sell stop arms when the market falls to 105; this bar reaches 90 first.
  exec.submitOrder(o);
  exec.onBar(kSym, Price::fromDouble(110.0), Price::fromDouble(90.0),
             Price::fromDouble(105.0));

  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_LE(exec.fills()[0].price.toDouble(), 105.0)
      << "a triggered stop-limit sell booked a price above its own trigger";
}

// Green control for area 3, second half. The bound is one-sided: a protective stop still
// pays the adverse side of the bar. A sell stop armed at 95 on a bar that
// traded down to 90 books 90, not the 95 it would have liked.
TEST(BacktestFillRealism, StopSellStillPaysTheAdverseExtreme)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  exec.submitOrder(
      conditionalOrder(1, kSym, Side::SELL, OrderType::STOP_MARKET, 95.0, 1.0));
  exec.onBar(kSym, Price::fromDouble(110.0), Price::fromDouble(90.0),
             Price::fromDouble(105.0));

  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 90.0);
}

// ===========================================================================
// 4. Queue model on bar data. processPendingOrders() skips every LIMIT while
//    the tracker is enabled and stepBarPrice() never drives the tracker, so
//    every canned venue preset (all of them set QueueModel::FULL) makes
//    resting limits unfillable on bars.
// ===========================================================================

// A limit buy at 99 rests behind 10 lots. The bar trades a full point through
// the level (low 98), so the queue ahead is gone and the order fills at 99 as
// a maker.
TEST(BacktestFillRealism, RestingLimitFillsOnBarDataWithQueueModel)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::FULL, 8);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 99.0, 5.0));
  ASSERT_EQ(exec.fills().size(), 0u) << "the order arrived behind the level";

  exec.onBar(kSym, Price::fromDouble(100.0), Price::fromDouble(98.0),
             Price::fromDouble(99.5));

  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 1), 5.0)
      << "the bar traded through 99 and the resting limit never filled";
  ASSERT_FALSE(exec.fills().empty());
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 99.0);
  EXPECT_TRUE(exec.fills()[0].isMaker);
}

// The canned presets are the reason this matters: binance_um_futures wires
// QueueModel::FULL, so the same bar has to fill the same order there.
TEST(BacktestFillRealism, VenueStackPresetFillsRestingLimitOnBarData)
{
  VenueStack stack = VenueStack::binance_um_futures(/*accountId=*/1, /*equity=*/100'000.0);
  SimulatedExecutor& exec = stack.executor();

  bool rejected = false;
  exec.setOrderEventCallback(
      [&rejected](const OrderEvent& ev)
      {
        if (ev.status == OrderEventStatus::REJECTED)
        {
          rejected = true;
        }
      });

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 99.0, 5.0));
  ASSERT_FALSE(rejected) << "the order never reached the book";

  exec.onBar(kSym, Price::fromDouble(100.0), Price::fromDouble(98.0),
             Price::fromDouble(99.5));

  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 1), 5.0);
  ASSERT_FALSE(exec.fills().empty());
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 99.0);
  EXPECT_TRUE(exec.fills()[0].isMaker);
}

// Green control for area 4, first half: touching the price is not trading
// through it. A bar whose low is exactly our bid leaves the queue ahead
// untouched, so nothing fills — a queue model that fills every resting limit
// on every bar is not a queue model.
TEST(BacktestFillRealism, BarThatOnlyTouchesThePriceLeavesTheQueueUnfilled)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::FULL, 8);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 99.0, 5.0));
  exec.onBar(kSym, Price::fromDouble(100.0), Price::fromDouble(99.0),
             Price::fromDouble(99.5));

  EXPECT_EQ(exec.fills().size(), 0u);
}

// Green control for area 4, second half: with no queue model the same resting
// limit fills at its posted price as it does today.
TEST(BacktestFillRealism, RestingLimitStillFillsOnBarDataWithoutQueueModel)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 99.0, 5.0));
  exec.onBar(kSym, Price::fromDouble(100.0), Price::fromDouble(98.0),
             Price::fromDouble(99.5));

  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 99.0);
  EXPECT_DOUBLE_EQ(exec.fills()[0].quantity.toDouble(), 5.0);
  EXPECT_TRUE(exec.fills()[0].isMaker);
}

// ===========================================================================
// 5. A second run() reports the second run. result() rebuilds from the
//    executor's fill vector, which no run path ever clears.
// ===========================================================================

// Two identical runs of the same three bars: the second result must describe
// one run, not two. Refusing the second run with an exception is the other
// acceptable answer; silently returning the sum is not.
TEST(BacktestFillRealism, SecondRunDoesNotAccumulateTheFirstRun)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  OneShotBuyStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;
  strat.fireOn = 0;

  BacktestRunner runner;
  runner.setStrategy(&strat);

  const BacktestResult first = runner.runBars(referenceBars(sym));
  ASSERT_EQ(first.fills().size(), 1u);

  strat.rearm();
  try
  {
    const BacktestResult second = runner.runBars(referenceBars(sym));
    EXPECT_EQ(second.fills().size(), first.fills().size())
        << "the second run reported both runs' fills";
  }
  catch (const std::exception&)
  {
    SUCCEED() << "a second run is refused outright, which is the other "
                 "documented answer";
  }
}

// The timestamps have to move with the run. The second pass replays bars one
// day later, so its first fill cannot carry the first pass's timestamp.
TEST(BacktestFillRealism, SecondRunReportsItsOwnTimestamps)
{
  constexpr int64_t kDayNs = 86'400'000'000'000LL;

  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  OneShotBuyStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;
  strat.fireOn = 0;

  BacktestRunner runner;
  runner.setStrategy(&strat);

  const BacktestResult first = runner.runBars(referenceBars(sym));
  ASSERT_EQ(first.fills().size(), 1u);
  const int64_t firstStart = first.fills().front().timestampNs.raw();

  std::vector<BarEvent> later = referenceBars(sym);
  for (auto& bar : later)
  {
    bar.bar.startTime += std::chrono::nanoseconds{kDayNs};
    bar.bar.endTime += std::chrono::nanoseconds{kDayNs};
  }

  strat.rearm();
  try
  {
    const BacktestResult second = runner.runBars(later);
    ASSERT_FALSE(second.fills().empty());
    EXPECT_GE(second.fills().front().timestampNs.raw(), firstStart + kDayNs)
        << "the second run's result starts at the first run's first fill";
  }
  catch (const std::exception&)
  {
    SUCCEED() << "a second run is refused outright";
  }
}

// Green control for area 5. A single run reports exactly what it did: one
// fill, timestamped inside the bar range it was given.
TEST(BacktestFillRealism, SingleRunReportsItsOwnFills)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  OneShotBuyStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;
  strat.fireOn = 0;

  BacktestRunner runner;
  runner.setStrategy(&strat);

  const BacktestResult res = runner.runBars(referenceBars(sym));
  ASSERT_EQ(res.fills().size(), 1u);
  EXPECT_GT(res.fills().front().timestampNs.raw(), 0);
  EXPECT_LE(res.fills().back().timestampNs.raw(), 3 * kMinuteNs);
}

// ===========================================================================
// 6. Hardening. The tests above pin what the fill model must do; the ones
//    below pin the places where a working model can still be got wrong --
//    the gates a held order has to pass at the open, the pairing of the
//    callback window, the arithmetic of the walk, and what a second run is
//    allowed to inherit.
// ===========================================================================

namespace
{

// Records every order event, so a test can ask what the executor decided
// rather than only what it filled.
class EventLog
{
 public:
  void attach(SimulatedExecutor& exec)
  {
    exec.setOrderEventCallback([this](const OrderEvent& ev)
                               { events.push_back(ev); });
  }

  size_t count(OrderEventStatus status) const
  {
    size_t n = 0;
    for (const auto& ev : events)
    {
      if (ev.status == status)
      {
        ++n;
      }
    }
    return n;
  }

  bool has(OrderId id, OrderEventStatus status) const
  {
    for (const auto& ev : events)
    {
      if (ev.order.id == id && ev.status == status)
      {
        return true;
      }
    }
    return false;
  }

  std::string reasonFor(OrderId id) const
  {
    for (const auto& ev : events)
    {
      if (ev.order.id == id && ev.status == OrderEventStatus::REJECTED)
      {
        return ev.rejectReason;
      }
    }
    return {};
  }

  std::vector<OrderEvent> events;
};

// A policy that lets exactly one order through per minute.
RateLimitPolicy oneSubmitPerMinute()
{
  RateLimitPolicy policy;
  policy.addBucket("orders", 60'000'000'000LL, /*capacity=*/1);
  return policy;
}

// Emits `count` market buys from the bar callback of bar `fireOn`, then
// optionally cancels every order of that symbol -- from the same callback,
// while all of them are still held.
class BurstStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  SymbolId target{0};
  size_t fireOn{0};
  size_t count{1};
  bool cancelAllSameBar{false};
  size_t bars{0};

 protected:
  void onSymbolBar(SymbolContext& /*ctx*/, const BarEvent& /*ev*/) override
  {
    if (bars == fireOn)
    {
      for (size_t i = 0; i < count; ++i)
      {
        emitMarketBuy(target, Quantity::fromDouble(1.0));
      }
      if (cancelAllSameBar)
      {
        emitCancelAll(target);
      }
    }
    ++bars;
  }
};

// Throws out of the bar callback once, the way a strategy with a bug does.
class ThrowingStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

 protected:
  void onSymbolBar(SymbolContext& /*ctx*/, const BarEvent& /*ev*/) override
  {
    throw std::runtime_error("strategy failed inside the bar callback");
  }
};

// A plain market-data subscriber -- not the strategy -- that sends an order
// from onBar. It reaches the executor through the same window.
class SubmittingSubscriber : public IMarketDataSubscriber
{
 public:
  SubmittingSubscriber(SimulatedExecutor& exec, OrderId id) : _exec(exec), _id(id) {}

  SubscriberId id() const override { return 0xB0B0u; }

  void onBar(const BarEvent& ev) override
  {
    if (_sent)
    {
      return;
    }
    _sent = true;
    _exec.submitOrder(marketOrder(_id, ev.symbol, Side::BUY, 1.0));
  }

 private:
  SimulatedExecutor& _exec;
  OrderId _id;
  bool _sent{false};
};

// In-memory reader, so run() and start() can be driven without a tape.
class VectorReader : public replay::IMultiSegmentReader
{
 public:
  explicit VectorReader(std::vector<replay::ReplayEvent> events) : _events(std::move(events)) {}

  uint64_t forEach(EventCallback callback) override
  {
    uint64_t n = 0;
    for (const auto& ev : _events)
    {
      if (!callback(ev))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  uint64_t forEachFrom(int64_t startNs, EventCallback callback) override
  {
    uint64_t n = 0;
    for (const auto& ev : _events)
    {
      if (ev.timestamp_ns < startNs)
      {
        continue;
      }
      if (!callback(ev))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  const std::vector<replay::SegmentInfo>& segments() const override { return _segments; }
  uint64_t totalEvents() const override { return _events.size(); }

 private:
  std::vector<replay::ReplayEvent> _events;
  std::vector<replay::SegmentInfo> _segments;
};

std::vector<replay::ReplayEvent> tradeStream(SymbolId sym, size_t count)
{
  std::vector<replay::ReplayEvent> events;
  for (size_t i = 0; i < count; ++i)
  {
    replay::ReplayEvent ev{};
    ev.type = replay::EventType::Trade;
    ev.timestamp_ns = static_cast<int64_t>((i + 1) * 1'000'000);
    ev.trade.symbol_id = sym;
    ev.trade.price_raw = Price::fromDouble(100.0).raw();
    ev.trade.qty_raw = Quantity::fromDouble(1.0).raw();
    ev.trade.side = 1;
    ev.trade.exchange_ts_ns = ev.timestamp_ns;
    events.push_back(ev);
  }
  return events;
}

// Buys once on the first trade it sees, so one reader pass produces one fill.
class FirstTradeBuyStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  SymbolId target{0};
  std::atomic<size_t> trades{0};

  void rearm() { trades.store(0); }

 protected:
  void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& /*ev*/) override
  {
    if (trades.fetch_add(1) == 0)
    {
      emitMarketBuy(target, Quantity::fromDouble(1.0));
    }
  }
};

}  // namespace

// --- the gates a held order still has to pass at the open ------------------

// Two orders are held from one callback and released into a venue that accepts
// one submit per minute. The hold is a delay, not a way around the venue: the
// first order fills at the 106 open and the second comes back rate-limited.
TEST(BacktestFillRealism, HeldOrdersAreReleasedThroughTheRateLimit)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  EventLog log;
  log.attach(exec);
  exec.setRateLimitPolicy(oneSubmitPerMinute());

  exec.beginBarCallbackWindow();
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 1.0));
  exec.submitOrder(marketOrder(2, kSym, Side::BUY, 1.0));
  exec.endBarCallbackWindow();
  ASSERT_EQ(exec.heldOrderCount(), 2u);

  exec.onBar(kSym, Price::fromDouble(106.0), Price::fromDouble(108.0),
             Price::fromDouble(104.0), Price::fromDouble(107.0));

  EXPECT_EQ(exec.fills().size(), 1u) << "the release skipped the rate limit";
  EXPECT_EQ(log.count(OrderEventStatus::REJECTED_RATE_LIMIT), 1u);
  ASSERT_FALSE(exec.fills().empty());
  EXPECT_DOUBLE_EQ(exec.fills().front().price.toDouble(), 106.0);
}

// Same two orders, and now it matters which is which: the venue takes them in
// the order the strategy sent them, so order 1 is the one that fills and order
// 2 is the one that is refused.
TEST(BacktestFillRealism, HeldOrdersReachTheVenueInArrivalOrder)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  EventLog log;
  log.attach(exec);
  exec.setRateLimitPolicy(oneSubmitPerMinute());

  exec.beginBarCallbackWindow();
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 1.0));
  exec.submitOrder(marketOrder(2, kSym, Side::BUY, 1.0));
  exec.endBarCallbackWindow();

  exec.onBar(kSym, Price::fromDouble(106.0), Price::fromDouble(108.0),
             Price::fromDouble(104.0), Price::fromDouble(107.0));

  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_EQ(exec.fills().front().orderId, 1u)
      << "the hold list was drained back to front";
  EXPECT_TRUE(log.has(2, OrderEventStatus::REJECTED_RATE_LIMIT));
}

// Reduce-only is evaluated at the open against the position as it stands
// there. A long of 1.0 and a held reduce-only sell of 5.0 trades 1.0, not 5.0.
TEST(BacktestFillRealism, HeldReduceOnlyOrderIsTruncatedAtTheOpen)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  exec.onBar(kSym, Price::fromDouble(100.0), Price::fromDouble(100.0),
             Price::fromDouble(100.0), Price::fromDouble(100.0));
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 1.0));
  ASSERT_DOUBLE_EQ(filledQty(exec.fills(), 1), 1.0);

  Order reducer = marketOrder(2, kSym, Side::SELL, 5.0);
  reducer.flags.reduceOnly = true;
  exec.beginBarCallbackWindow();
  exec.submitOrder(reducer);
  exec.endBarCallbackWindow();

  exec.onBar(kSym, Price::fromDouble(106.0), Price::fromDouble(108.0),
             Price::fromDouble(104.0), Price::fromDouble(107.0));

  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 2), 1.0)
      << "the release skipped the reduce-only truncation";
}

// Self-trade prevention likewise fires at the open, against what is resting
// there: a held buy at 101 that would cross our own resting sell at 100 is
// refused, not filled.
TEST(BacktestFillRealism, HeldOrderStillFacesSelfTradePrevention)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  EventLog log;
  log.attach(exec);
  exec.setSTPMode(STPMode::CancelNewest);

  exec.submitOrder(limitOrder(1, kSym, Side::SELL, 100.0, 1.0));

  exec.beginBarCallbackWindow();
  exec.submitOrder(limitOrder(2, kSym, Side::BUY, 101.0, 1.0));
  exec.endBarCallbackWindow();

  exec.onBar(kSym, Price::fromDouble(100.0), Price::fromDouble(100.0),
             Price::fromDouble(100.0), Price::fromDouble(100.0));

  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 2), 0.0)
      << "the release skipped self-trade prevention";
  EXPECT_EQ(log.reasonFor(2), "stp_cancel_newest");
}

// --- the window itself -----------------------------------------------------

// An order the strategy cancelled along with everything else never reaches the
// venue, even though it was still held when the cancel came: none of the three
// buys fills at the 106 open.
TEST(BacktestFillRealism, CancelAllInsideTheCallbackAlsoDropsTheHeldOrders)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  BurstStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;
  strat.fireOn = 0;
  strat.count = 3;
  strat.cancelAllSameBar = true;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runBars(referenceBars(sym));

  EXPECT_EQ(runner.executor().fills().size(), 0u)
      << "cancelAll did not reach the orders that were still held";
  EXPECT_EQ(runner.executor().heldOrderCount(), 0u);
}

// The window covers every consumer of the bar, not only the strategy: an order
// a market-data subscriber sends from onBar is held to the next open too.
TEST(BacktestFillRealism, SubscriberOrderIsHeldToTheNextOpen)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  BacktestRunner runner;
  SubmittingSubscriber sub(runner.executor(), /*id=*/777);
  runner.addMarketDataSubscriber(&sub);
  runner.runBars(referenceBars(sym));

  ASSERT_EQ(runner.executor().fills().size(), 1u);
  const double px = runner.executor().fills().front().price.toDouble();
  EXPECT_NE(px, 105.0) << "the subscriber's order filled at the close it was shown";
  EXPECT_DOUBLE_EQ(px, 106.0);
}

// The window has to be closed as many times as it was opened. A run with no
// strategy attached opens one per bar and must leave none behind, or every
// order submitted afterwards is held for an open that never comes.
TEST(BacktestFillRealism, WindowIsPairedOnARunWithNoStrategy)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  BacktestRunner runner;
  runner.runBars(referenceBars(sym));

  EXPECT_FALSE(runner.executor().barCallbackWindowOpen())
      << "the run left the bar-callback window open";

  runner.executor().submitOrder(marketOrder(42, sym, Side::BUY, 1.0));
  EXPECT_EQ(runner.executor().heldOrderCount(), 0u);
  EXPECT_EQ(runner.executor().fills().size(), 1u)
      << "an order submitted after the run was held instead of sent";
}

// The other half of the pairing, and the one the code does not do yet: a
// callback that throws unwinds past the close, so the window has to be closed
// by a scope guard rather than by the statement after the call. Enable this
// once the runner owns the window through RAII.
TEST(BacktestFillRealism, WindowIsPairedWhenABarCallbackThrows)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  ThrowingStrategy strat(1, std::vector<SymbolId>{sym}, reg);

  BacktestRunner runner;
  runner.setStrategy(&strat);
  EXPECT_THROW(runner.runBars(referenceBars(sym)), std::runtime_error);

  EXPECT_FALSE(runner.executor().barCallbackWindowOpen())
      << "a throwing bar callback left the window open";

  runner.executor().onBar(kSym, Price::fromDouble(100.0), Price::fromDouble(100.0),
                          Price::fromDouble(100.0), Price::fromDouble(100.0));
  runner.executor().submitOrder(marketOrder(42, kSym, Side::BUY, 1.0));
  EXPECT_EQ(runner.executor().heldOrderCount(), 0u);
}

// --- the arithmetic of the walk -------------------------------------------

// The walk's notional is money, so it goes through the widened
// Quantity * Price -> Volume path and not through a double. At a crypto
// notional the two disagree: asks 67123.45678901 x 0.2, 67123.45678903 x 0.2
// and 67123.45678905 x 0.1 fill a buy of 0.5 at raw 6712345678900 exactly --
// each level's notional truncates down, and the average lands one raw unit
// below the cheapest level rather than wherever floating point rounds it.
TEST(BacktestFillRealism, LadderVwapIsExactAtCryptoNotionals)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{67000.0, 10.0}},
             {{67123.45678901, 0.2}, {67123.45678903, 0.2}, {67123.45678905, 0.1}});
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 0.5));

  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 1), 0.5);
  EXPECT_EQ(exec.fills().front().price.raw(), 6712345678900)
      << "the walk's notional went through a double";
}

// The deepest-level rule is not a property of the buy side. Bids 99 x 1 and
// 98 x 1 against a sell of 10: the whole order prints at 98, the deepest level
// the feed showed -- not the 98.5 average of the two levels it could see.
TEST(BacktestFillRealism, SellPastTheLadderPaysTheDeepestLevelForTheWholeOrder)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 1.0}, {98.0, 1.0}}, {{101.0, 10.0}});
  exec.submitOrder(marketOrder(1, kSym, Side::SELL, 10.0));

  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 1), 10.0);
  EXPECT_NEAR(vwap(exec.fills(), 1), 98.0, 1e-9)
      << "the sell averaged over the visible levels instead of paying the deepest";
}

// A conditional refused because it triggered past its own limit must not pay
// for the attempt. The stop-limit below is armed at 97.5 with a limit of 98,
// so it rests; the ladder it did not trade against is untouched, and the next
// taker still gets the 99 touch.
TEST(BacktestFillRealism, RefusedStopLimitEatsNoDepth)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 1.0}, {98.0, 2.0}, {97.0, 5.0}}, {{101.0, 10.0}});

  Order stopLimit = conditionalOrder(1, kSym, Side::SELL, OrderType::STOP_LIMIT, 97.5, 3.0);
  stopLimit.price = Price::fromDouble(98.0);
  exec.submitOrder(stopLimit);

  // A print at 97.5 arms it: the market fell through the trigger, and its 98
  // limit is above where it can trade.
  exec.onTrade(kSym, Price::fromDouble(97.5), /*isBuy=*/false);
  ASSERT_DOUBLE_EQ(filledQty(exec.fills(), 1), 0.0);

  exec.submitOrder(marketOrder(2, kSym, Side::SELL, 1.0));
  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 2), 1.0);
  EXPECT_NEAR(vwap(exec.fills(), 2), 99.0, 1e-9)
      << "the refused stop-limit took the top of the book with it";
}

// --- two of our own orders at one price ------------------------------------

// The synthetic print a bar step hands the queue model is sized to the order
// standing furthest back, not to whichever of ours is seen first. Order 1 is
// alone at 99 when it arrives (queue ahead 0, size 2); order 2 arrives after
// the level has grown to 10, so it waits behind all of it with size 5. A bar
// through 99 prints 15: order 1 fills its 2, and order 2 gets the 3 that are
// left after its own queue.
TEST(BacktestFillRealism, QueuePrintReachesTheOrderStandingFurthestBack)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::FULL, 8);

  pushLadder(exec, kSym, {{99.0, 0.0}}, {{101.0, 10.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 99.0, 2.0));

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(limitOrder(2, kSym, Side::BUY, 99.0, 5.0));
  ASSERT_EQ(exec.fills().size(), 0u);

  exec.onBar(kSym, Price::fromDouble(100.0), Price::fromDouble(100.0),
             Price::fromDouble(98.0), Price::fromDouble(99.5));

  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 1), 2.0);
  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 2), 3.0)
      << "the print stopped at the first order at the level";
}

// --- repeatability ---------------------------------------------------------

// Two runs over one executor with jittered acks draw the same sequence: the
// seeded generators go back to their seeds with everything else the run left
// behind, or a backtest stops being reproducible.
TEST(BacktestFillRealism, AckJitterRepeatsAcrossRuns)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setSubmitAckLatency(10'000'000LL, 5'000'000LL);
  exec.setCancelAckLatency(7'000'000LL, 4'000'000LL);

  std::vector<int64_t> observed;
  exec.setOrderEventCallback(
      [&observed](const OrderEvent& ev)
      {
        if (ev.status == OrderEventStatus::ACCEPTED ||
            ev.status == OrderEventStatus::CANCELED)
        {
          observed.push_back(ev.exchangeTsNs.raw());
        }
      });

  // Steps the clock in 250 us ticks and pumps the executor, so each ack lands
  // in a bucket the drawn latency picks out.
  auto pump = [&exec, &clock](int64_t& nowNs)
  {
    for (int i = 0; i < 80; ++i)
    {
      nowNs += 250'000LL;
      clock.advanceTo(UnixNanos::fromRaw(nowNs));
      exec.onTrade(kSym, Price::fromDouble(100.0), /*isBuy=*/true);
    }
  };

  auto script = [&](std::vector<int64_t>& out)
  {
    observed.clear();
    int64_t nowNs = 0;
    for (OrderId id = 1; id <= 6; ++id)
    {
      exec.submitOrder(limitOrder(id, kSym, Side::BUY, 50.0, 1.0));
      pump(nowNs);
      exec.cancelOrder(id);
      pump(nowNs);
    }
    out = observed;
  };

  std::vector<int64_t> first;
  script(first);
  ASSERT_FALSE(first.empty());

  exec.reset();
  clock.reset();
  std::vector<int64_t> second;
  script(second);

  EXPECT_EQ(second, first) << "the second run drew a different latency sequence";
}

// The same for the iceberg size jitter: the refreshed tranches of run two are
// the tranches of run one, tranche for tranche.
TEST(BacktestFillRealism, IcebergJitterRepeatsAcrossRuns)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::FULL, 8);
  exec.setIcebergRefreshLatency(0);
  exec.setIcebergSizeRandomisationPct(0.5);
  exec.setIcebergJitterSeed(1234);

  auto script = [&](std::vector<double>& out)
  {
    out.clear();
    pushLadder(exec, kSym, {{99.0, 0.0}}, {{101.0, 10.0}});
    Order ice = limitOrder(1, kSym, Side::BUY, 99.0, 1.0);
    ice.type = OrderType::ICEBERG;
    ice.quantity = Quantity::fromDouble(6.0);
    ice.visibleQuantity = Quantity::fromDouble(1.0);
    exec.submitOrder(ice);

    for (int i = 0; i < 8; ++i)
    {
      exec.onTrade(kSym, Price::fromDouble(99.0), Quantity::fromDouble(1.0),
                   /*isBuy=*/false);
    }
    for (const auto& f : exec.fills())
    {
      out.push_back(f.quantity.toDouble());
    }
  };

  std::vector<double> first;
  script(first);
  ASSERT_FALSE(first.empty());

  exec.reset();
  clock.reset();
  std::vector<double> second;
  script(second);

  EXPECT_EQ(second, first) << "the second run drew a different tranche sequence";
}

// --- what a second run inherits -------------------------------------------

// A run can end with an order still held -- its symbol never got another bar.
// The next run must not be the open it was waiting for.
TEST(BacktestFillRealism, AHeldOrderDoesNotSurviveIntoTheNextRun)
{
  SymbolRegistry reg;
  const SymbolId driver = addSymbol(reg, "BTCUSDT");
  const SymbolId other = addSymbol(reg, "ETHUSDT");

  OneShotBuyStrategy strat(1, std::vector<SymbolId>{driver, other}, reg);
  strat.target = other;
  strat.fireOn = 0;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runBars({makeBar(driver, 100.0, 110.0, 90.0, 105.0, 0),
                  makeBar(driver, 106.0, 108.0, 104.0, 107.0, kMinuteNs)});
  ASSERT_EQ(runner.executor().heldOrderCount(), 1u);
  ASSERT_EQ(runner.executor().fills().size(), 0u);

  // Run two trades the symbol the held order was waiting for, and the strategy
  // sends nothing of its own.
  strat.rearm();
  strat.fireOn = 99;
  runner.runBars({makeBar(other, 200.0, 210.0, 190.0, 205.0, 2 * kMinuteNs)});

  EXPECT_EQ(runner.executor().fills().size(), 0u)
      << "the previous run's held order was released into this one";
  EXPECT_EQ(runner.executor().heldOrderCount(), 0u);
}

// The reset belongs to every entry point, not only to the bar path: run() over
// a reader reports its own run.
TEST(BacktestFillRealism, SecondReaderRunDoesNotAccumulate)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  FirstTradeBuyStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;

  BacktestRunner runner;
  runner.setStrategy(&strat);

  VectorReader reader(tradeStream(sym, 5));
  const BacktestResult first = runner.run(reader);
  ASSERT_EQ(first.fills().size(), 1u);

  strat.rearm();
  const BacktestResult second = runner.run(reader);
  EXPECT_EQ(second.fills().size(), first.fills().size())
      << "the second reader run reported both runs' fills";
}

// And to the interactive path: a second start() is a second run.
TEST(BacktestFillRealism, SecondInteractiveRunDoesNotAccumulate)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  FirstTradeBuyStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  VectorReader reader(tradeStream(sym, 5));

  auto drive = [&runner, &reader]()
  {
    std::thread worker([&]()
                       { runner.start(reader); });
    for (int i = 0; i < 20'000 && !runner.isPaused(); ++i)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    runner.resume();
    for (int i = 0; i < 20'000 && !runner.isFinished(); ++i)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    runner.stop();
    worker.join();
  };

  drive();
  const size_t firstFills = runner.executor().fills().size();
  ASSERT_EQ(firstFills, 1u);

  strat.rearm();
  drive();
  EXPECT_EQ(runner.executor().fills().size(), firstFills)
      << "the second interactive run reported both runs' fills";
}

// The queue the previous run left behind goes with it. Run one leaves an order
// resting at 99; run two's order must queue behind the level as the venue
// publishes it, not behind a ghost from the run before.
TEST(BacktestFillRealism, QueueEntriesDoNotSurviveAReset)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::FULL, 8);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 99.0, 5.0));
  ASSERT_EQ(exec.fills().size(), 0u);

  exec.reset();
  clock.reset();

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(limitOrder(2, kSym, Side::BUY, 99.0, 5.0));
  exec.onBar(kSym, Price::fromDouble(100.0), Price::fromDouble(100.0),
             Price::fromDouble(98.0), Price::fromDouble(99.5));

  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 2), 5.0)
      << "the previous run's order was still standing in the queue";
}
