/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Unrealized PnL on the strategy context.
//
// The context carried an average entry price that nothing in the engine ever
// wrote -- there was no source for it, since the position manager interface
// only reported a quantity -- so it read back as zero and the PnL came out as
// position times mark. A long unit bought at 100 and marked at 110 reported
// +110 instead of +10. A rule of the form "close when the loss passes X"
// therefore never fired on a long and fired on the first tick of a short.

#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/position/multi_mode_position_tracker.h"
#include "flox/position/position_tracker.h"
#include "flox/strategy/strategy.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{

SymbolId registerSymbol(SymbolRegistry& registry, const std::string& name)
{
  SymbolInfo info;
  info.exchange = "TEST";
  info.symbol = name;
  info.tickSize = Price::fromDouble(0.01);
  return registry.registerSymbol(info);
}

Order makeOrder(OrderId id, SymbolId sym, Side side, double price, double qty)
{
  Order o{};
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

class ObservingStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  void start() override {}
  void stop() override {}

  std::vector<std::optional<double>> seenPnl;
  std::vector<std::optional<Price>> seenEntry;

 protected:
  void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override
  {
    seenEntry.push_back(c.avgEntryPrice);
    seenPnl.push_back(c.unrealizedPnl(ev.trade.price));
  }
};

TradeEvent tradeAt(SymbolId sym, double price)
{
  TradeEvent ev;
  ev.trade.symbol = sym;
  ev.trade.price = Price::fromDouble(price);
  ev.trade.quantity = Quantity::fromDouble(1.0);
  return ev;
}

}  // namespace

TEST(StrategyUnrealizedPnl, LongReportsProfitNotNotional)
{
  SymbolRegistry registry;
  const SymbolId sym = registerSymbol(registry, "SYM1");

  PositionTracker positions{2};
  ObservingStrategy strategy{1, std::vector<SymbolId>{sym}, registry};
  strategy.setPositionManager(&positions);

  positions.onOrderFilled(makeOrder(1, sym, Side::BUY, 100.0, 1.0));
  strategy.onTrade(tradeAt(sym, 110.0));

  ASSERT_EQ(strategy.seenPnl.size(), 1u);
  ASSERT_TRUE(strategy.seenEntry[0].has_value());
  EXPECT_NEAR(strategy.seenEntry[0]->toDouble(), 100.0, 0.01);
  ASSERT_TRUE(strategy.seenPnl[0].has_value());
  EXPECT_NEAR(*strategy.seenPnl[0], 10.0, 0.01);
}

TEST(StrategyUnrealizedPnl, ShortReportsProfitNotNotional)
{
  SymbolRegistry registry;
  const SymbolId sym = registerSymbol(registry, "SYM1");

  PositionTracker positions{2};
  ObservingStrategy strategy{1, std::vector<SymbolId>{sym}, registry};
  strategy.setPositionManager(&positions);

  positions.onOrderFilled(makeOrder(1, sym, Side::SELL, 100.0, 1.0));
  strategy.onTrade(tradeAt(sym, 110.0));

  ASSERT_EQ(strategy.seenPnl.size(), 1u);
  ASSERT_TRUE(strategy.seenPnl[0].has_value());
  EXPECT_NEAR(*strategy.seenPnl[0], -10.0, 0.01);
}

// The practical shape of the bug: a stop rule on a long. At an entry of
// 100,000 and a mark of 99,000 the loss is 1,000, well past a 500 threshold.
// Reported as position times mark it came out as +99,000 and the rule never
// fired.
TEST(StrategyUnrealizedPnl, LossThresholdFiresOnALong)
{
  SymbolRegistry registry;
  const SymbolId sym = registerSymbol(registry, "SYM1");

  PositionTracker positions{2};
  ObservingStrategy strategy{1, std::vector<SymbolId>{sym}, registry};
  strategy.setPositionManager(&positions);

  positions.onOrderFilled(makeOrder(1, sym, Side::BUY, 100000.0, 1.0));
  strategy.onTrade(tradeAt(sym, 99000.0));

  ASSERT_TRUE(strategy.seenPnl[0].has_value());
  EXPECT_NEAR(*strategy.seenPnl[0], -1000.0, 0.01);
  EXPECT_LT(*strategy.seenPnl[0], -500.0);
}

// Nothing to report when the position is flat: the PnL is zero, not unknown.
TEST(StrategyUnrealizedPnl, FlatPositionReportsZero)
{
  SymbolRegistry registry;
  const SymbolId sym = registerSymbol(registry, "SYM1");

  PositionTracker positions{2};
  ObservingStrategy strategy{1, std::vector<SymbolId>{sym}, registry};
  strategy.setPositionManager(&positions);

  strategy.onTrade(tradeAt(sym, 110.0));

  ASSERT_TRUE(strategy.seenPnl[0].has_value());
  EXPECT_DOUBLE_EQ(*strategy.seenPnl[0], 0.0);
  EXPECT_FALSE(strategy.seenEntry[0].has_value());
}

// A position manager written against the old interface reports no entry
// price. The context then says "unknown" rather than handing back the
// notional dressed up as profit.
TEST(StrategyUnrealizedPnl, ManagerWithoutCostBasisReportsUnknown)
{
  class QuantityOnlyManager : public IPositionManager
  {
   public:
    using IPositionManager::IPositionManager;
    void start() override {}
    void stop() override {}
    Quantity getPosition(SymbolId) const override { return Quantity::fromDouble(1.0); }
  };

  SymbolRegistry registry;
  const SymbolId sym = registerSymbol(registry, "SYM1");

  QuantityOnlyManager positions{2};
  ObservingStrategy strategy{1, std::vector<SymbolId>{sym}, registry};
  strategy.setPositionManager(&positions);

  strategy.onTrade(tradeAt(sym, 110.0));

  EXPECT_FALSE(strategy.seenEntry[0].has_value());
  EXPECT_FALSE(strategy.seenPnl[0].has_value());
}

// Both shipped managers answer the question.
TEST(StrategyUnrealizedPnl, MultiModeTrackerReportsItsEntryPrice)
{
  MultiModePositionTracker net{1, PositionAggregationMode::NET};
  EXPECT_FALSE(net.getAverageEntryPrice(100).has_value());

  net.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 2.0));
  net.onOrderFilled(makeOrder(2, 100, Side::BUY, 200.0, 2.0));

  auto entry = net.getAverageEntryPrice(100);
  ASSERT_TRUE(entry.has_value());
  EXPECT_NEAR(entry->toDouble(), 150.0, 0.01);
}
