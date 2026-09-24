/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The decisions behind the fill model, pinned where the acceptance suite in
// test_backtest_fill_realism.cpp leaves a choice open: what an order past the
// visible ladder pays, what the touch looks like after a walk, where an order
// held for the next bar's open goes when it is cancelled or belongs to another
// symbol, and what a queue model does with a limit it never registered.

#include "flox/aggregator/events/bar_event.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/engine/abstract_market_data_subscriber.h"
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

// Buys `qty` of `target` from the bar callback of bar `fireOn`, and cancels it
// in the same callback when `cancelSameBar` is set.
class ScriptedStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  SymbolId target{0};
  double qty{1.0};
  size_t fireOn{0};
  bool cancelSameBar{false};
  size_t bars{0};

 protected:
  void onSymbolBar(SymbolContext& /*ctx*/, const BarEvent& /*ev*/) override
  {
    if (bars == fireOn)
    {
      const OrderId id = emitMarketBuy(target, Quantity::fromDouble(qty));
      if (cancelSameBar)
      {
        emitCancel(id);
      }
    }
    ++bars;
  }
};

// A market-data subscriber that submits one order straight to the executor
// from its bar callback. Subscribers see the bar after the strategy does, and
// they are inside the same window.
class OrderingSubscriber : public IMarketDataSubscriber
{
 public:
  OrderingSubscriber(SubscriberId id, SimulatedExecutor& exec, SymbolId sym)
      : _id(id), _exec(exec), _sym(sym)
  {
  }

  SubscriberId id() const override { return _id; }

  void onBar(const BarEvent& /*ev*/) override
  {
    if (_sent)
    {
      return;
    }
    _sent = true;
    _exec.submitOrder(marketOrder(7, _sym, Side::BUY, 1.0));
  }

 private:
  SubscriberId _id;
  SimulatedExecutor& _exec;
  SymbolId _sym;
  bool _sent{false};
};

// Emits one market buy from the first bar callback and then throws out of it.
class ThrowingStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  SymbolId target{0};

 protected:
  void onSymbolBar(SymbolContext& /*ctx*/, const BarEvent& /*ev*/) override
  {
    emitMarketBuy(target, Quantity::fromDouble(1.0));
    throw std::runtime_error("strategy blew up inside onBar");
  }
};

}  // namespace

// Size past the visible ladder fills in full, at the deepest level the walk
// reached -- for the whole order, not just the excess. Asks 101 x 1, 102 x 1
// against a buy of 10: every lot pays 102.
TEST(BacktestFillModel, SizePastTheLadderPaysTheDeepestLevelForTheWholeOrder)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 1.0}, {102.0, 1.0}});
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 10.0));

  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].quantity.toDouble(), 10.0);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 102.0);
  EXPECT_FALSE(exec.fills()[0].isMaker);
}

// A walk that stops inside a level republishes the touch: the level is still
// there, with what is left of it.
TEST(BacktestFillModel, PartialLevelLeavesTheRestOfItAtTheTouch)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 4.0}, {102.0, 5.0}});
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 1.0));

  EXPECT_DOUBLE_EQ(exec.bestAskPrice(kSym).toDouble(), 101.0);

  // 3 left at 101, then 102: a buy of 4 pays (303 + 102) / 4 = 101.25.
  exec.submitOrder(marketOrder(2, kSym, Side::BUY, 4.0));
  ASSERT_EQ(exec.fills().size(), 2u);
  EXPECT_DOUBLE_EQ(exec.fills()[1].price.toDouble(), 101.25);
  EXPECT_DOUBLE_EQ(exec.bestAskPrice(kSym).toDouble(), 102.0);
}

// A side eaten to the last lot is empty until the next snapshot says otherwise.
TEST(BacktestFillModel, ExhaustedSideReportsNoTouch)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 2.0}});
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 2.0));

  EXPECT_DOUBLE_EQ(exec.bestAskPrice(kSym).toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(exec.bestBidPrice(kSym).toDouble(), 99.0);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 2.0}});
  EXPECT_DOUBLE_EQ(exec.bestAskPrice(kSym).toDouble(), 101.0);
}

// A resting limit provides the liquidity, so it consumes no depth: the book it
// posted into is untouched after it trades.
TEST(BacktestFillModel, MakerFillTakesNoDepth)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 5.0}}, {{101.0, 5.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 100.0, 1.0));
  ASSERT_EQ(exec.fills().size(), 0u);

  // The market comes to it: a snapshot whose ask is at its price.
  pushLadder(exec, kSym, {{99.0, 5.0}}, {{100.0, 5.0}});
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_TRUE(exec.fills()[0].isMaker);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 100.0);
  EXPECT_DOUBLE_EQ(exec.bestAskPrice(kSym).toDouble(), 100.0);
}

// A bar reports no depth, so a stale ladder must not be walked at prices the
// bar has already left behind: on bar data any size trades at the step price.
TEST(BacktestFillModel, BarStepDropsTheStaleLadder)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 1.0}}, {{101.0, 1.0}});
  exec.onBar(kSym, Price::fromDouble(110.0), Price::fromDouble(105.0),
             Price::fromDouble(108.0));
  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 50.0));

  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].quantity.toDouble(), 50.0);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 108.0);
}

// With a queue model on, a limit the tracker never registered still fills on
// the crossing rule: a stop-limit that rests when it triggers has no queue to
// wait behind, and skipping it would leave it resting for the whole run.
TEST(BacktestFillModel, TriggeredStopLimitFillsWithAQueueModelOn)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::FULL, 8);

  Order o;
  o.id = 1;
  o.symbol = kSym;
  o.side = Side::SELL;
  o.type = OrderType::STOP_LIMIT;
  o.quantity = Quantity::fromDouble(1.0);
  o.triggerPrice = Price::fromDouble(105.0);
  o.price = Price::fromDouble(104.0);
  exec.submitOrder(o);

  // Triggers on the low, cannot trade at 90 with a limit of 104, and rests.
  exec.onBar(kSym, Price::fromDouble(110.0), Price::fromDouble(90.0),
             Price::fromDouble(105.0));

  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 104.0);
}

// An order submitted and pulled inside one bar callback never reached the
// venue: the next bar's open must not release it.
TEST(BacktestFillModel, OrderCancelledInsideTheSameCallbackNeverFills)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  ScriptedStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;
  strat.fireOn = 0;
  strat.cancelSameBar = true;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runBars({makeBar(sym, 100.0, 110.0, 90.0, 105.0, 0),
                  makeBar(sym, 106.0, 108.0, 104.0, 107.0, kMinuteNs)});

  EXPECT_EQ(runner.executor().fills().size(), 0u);
}

// An order for a symbol whose bar never comes back is never released: it is
// still held when the run ends, and it neither fills nor trades on some other
// symbol's open.
TEST(BacktestFillModel, HeldOrderWaitsForItsOwnSymbolsOpen)
{
  SymbolRegistry reg;
  const SymbolId driver = addSymbol(reg, "BTCUSDT");
  const SymbolId other = addSymbol(reg, "ETHUSDT");

  ScriptedStrategy strat(1, std::vector<SymbolId>{driver, other}, reg);
  strat.target = other;
  strat.fireOn = 0;

  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runBars({makeBar(driver, 100.0, 110.0, 90.0, 105.0, 0),
                  makeBar(driver, 106.0, 108.0, 104.0, 107.0, kMinuteNs)});

  EXPECT_EQ(runner.executor().fills().size(), 0u);
  EXPECT_EQ(runner.executor().heldOrderCount(), 1u);
}

// A subscriber is inside the window too. It is shown the same walked bar the
// strategy is, so an order it submits is held for the next open like any
// other: 106, not the 105 close it was handed.
TEST(BacktestFillModel, ASubscribersOrderIsHeldForTheNextOpenAsWell)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  BacktestRunner runner;
  OrderingSubscriber sub(0xABCD, runner.executor(), sym);
  runner.addMarketDataSubscriber(&sub);

  runner.runBars({makeBar(sym, 100.0, 110.0, 90.0, 105.0, 0),
                  makeBar(sym, 106.0, 108.0, 104.0, 107.0, kMinuteNs)});

  const auto& fills = runner.executor().fills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_NE(fills[0].price.toDouble(), 105.0)
      << "the subscriber's order filled at the close of the bar it was shown";
  EXPECT_DOUBLE_EQ(fills[0].price.toDouble(), 106.0);
}

// A callback that throws must not leave the bar-callback window open. A window
// that never closes holds every order submitted after it for the rest of the
// run, and nothing reports that: the orders simply never fill.
TEST(BacktestFillModel, AThrowingBarCallbackClosesTheWindow)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  ThrowingStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;

  BacktestRunner runner;
  runner.setStrategy(&strat);

  EXPECT_THROW(runner.runBars({makeBar(sym, 100.0, 110.0, 90.0, 105.0, 0),
                               makeBar(sym, 106.0, 108.0, 104.0, 107.0, kMinuteNs)}),
               std::runtime_error);

  SimulatedExecutor& exec = runner.executor();
  EXPECT_FALSE(exec.barCallbackWindowOpen());

  // The consequence, not just the flag: an order submitted after the throw
  // reaches the book instead of being held for a bar open that never comes.
  exec.submitOrder(marketOrder(99, sym, Side::BUY, 1.0));
  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 99), 1.0);
}

// The window is opened for every bar, strategy or not: a run with only
// subscribers attached must close it too.
TEST(BacktestFillModel, ARunWithoutAStrategyLeavesNoWindowOpen)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  BacktestRunner runner;
  runner.runBars({makeBar(sym, 100.0, 110.0, 90.0, 105.0, 0),
                  makeBar(sym, 106.0, 108.0, 104.0, 107.0, kMinuteNs)});

  SimulatedExecutor& exec = runner.executor();
  EXPECT_FALSE(exec.barCallbackWindowOpen());

  exec.submitOrder(marketOrder(1, sym, Side::BUY, 1.0));
  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 1), 1.0);
  EXPECT_EQ(exec.heldOrderCount(), 0u);
}

// reset() puts a configured executor back to where a fresh one would be,
// without dropping the configuration.
TEST(BacktestFillModel, ResetClearsFillsAndOrdersButKeepsConfiguration)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::FULL, 8);

  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 99.0, 5.0));
  exec.submitOrder(marketOrder(2, kSym, Side::BUY, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);

  exec.reset();
  EXPECT_TRUE(exec.fills().empty());
  EXPECT_DOUBLE_EQ(exec.bestBidPrice(kSym).toDouble(), 0.0);

  // The queue model survived: the same order rests again instead of filling,
  // and the same bar takes it out.
  pushLadder(exec, kSym, {{99.0, 10.0}}, {{101.0, 10.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 99.0, 5.0));
  EXPECT_EQ(exec.fills().size(), 0u);
  exec.onBar(kSym, Price::fromDouble(100.0), Price::fromDouble(98.0),
             Price::fromDouble(99.5));
  EXPECT_DOUBLE_EQ(filledQty(exec.fills(), 1), 5.0);
}
