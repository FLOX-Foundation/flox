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
#include "flox/engine/symbol_registry.h"
#include "flox/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory_resource>
#include <stdexcept>
#include <string>
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
