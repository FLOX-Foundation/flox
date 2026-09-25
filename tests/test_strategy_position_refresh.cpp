/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What a market-data tick pays for the position it usually has not changed.
//
// Strategy::refreshPosition() runs on every trade, every book update and
// every bar, while the symbol lock is held, and it makes two separate calls
// into the position manager: getPosition() and getAverageEntryPrice(). On
// PositionTracker each of those takes the tracker's one mutex and walks the
// symbol's lot deque -- position() sums every lot, avgEntryPrice() makes a
// second pass over all of them in double. So one tick is two acquisitions of
// a mutex the execution thread also wants, and 2 * O(open lots) of work, for
// a pair of values that only ever change when a fill lands.
//
// IPositionManager is the seam, so the cost is countable: every acquisition
// of the tracker's mutex is one call through this interface.

#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/position/abstract_position_manager.h"
#include "flox/strategy/strategy.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using namespace flox;

namespace
{

// Counts what a tick asks of the position manager. On PositionTracker each
// of these calls is one lock/unlock of the tracker mutex and one or two
// passes over the lot deque.
class CountingPositionManager : public IPositionManager
{
 public:
  CountingPositionManager() : IPositionManager(99) {}

  void start() override {}
  void stop() override {}

  Quantity getPosition(SymbolId) const override
  {
    ++queries;
    return _position;
  }

  std::optional<Price> getAverageEntryPrice(SymbolId) const override
  {
    ++queries;
    if (_position.isZero())
    {
      return std::nullopt;
    }
    return _entry;
  }

  void setFill(Quantity qty, Price entry)
  {
    _position = qty;
    _entry = entry;
  }

  mutable int queries{0};

 private:
  Quantity _position{};
  Price _entry{};
};

class CountingStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  std::vector<double> positionsSeen;

 protected:
  void onSymbolTrade(SymbolContext& ctx, const TradeEvent&) override
  {
    positionsSeen.push_back(ctx.position.toDouble());
  }

  void onSymbolBook(SymbolContext& ctx, const BookUpdateEvent&) override
  {
    positionsSeen.push_back(ctx.position.toDouble());
  }
};

SymbolId addSymbol(SymbolRegistry& reg, const std::string& spelling)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = spelling;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

TradeEvent makeTrade(SymbolId sym, double price, int64_t tsNs)
{
  TradeEvent ev{};
  ev.trade.symbol = sym;
  ev.trade.price = Price::fromDouble(price);
  ev.trade.quantity = Quantity::fromDouble(1.0);
  ev.trade.isBuy = true;
  ev.trade.exchangeTsNs = UnixNanos::fromRaw(tsNs);
  return ev;
}

}  // namespace

// The green control: the refresh does its job. Whatever is done about the
// cost, the context still has to carry the position the manager reports.
TEST(StrategyPositionRefresh, TheContextCarriesThePositionTheManagerReports)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  CountingPositionManager pm;
  CountingStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.setPositionManager(&pm);

  strat.onTrade(makeTrade(sym, 100.0, 1));
  ASSERT_EQ(strat.positionsSeen.size(), 1u);
  EXPECT_DOUBLE_EQ(strat.positionsSeen[0], 0.0);

  pm.setFill(Quantity::fromDouble(0.5), Price::fromDouble(100.0));
  strat.onTrade(makeTrade(sym, 101.0, 2));
  ASSERT_EQ(strat.positionsSeen.size(), 2u);
  EXPECT_DOUBLE_EQ(strat.positionsSeen[1], 0.5)
      << "the context must reflect the fill on the next tick";

  pm.setFill(Quantity{}, Price{});
  strat.onTrade(makeTrade(sym, 102.0, 3));
  ASSERT_EQ(strat.positionsSeen.size(), 3u);
  EXPECT_DOUBLE_EQ(strat.positionsSeen[2], 0.0) << "and the flattening after it";
}

// One tick, one question. Two calls per tick means two acquisitions of the
// tracker mutex on the market-data thread, each contending with the
// execution thread, for two values that come from the same state under the
// same lock.
//
// needs, if the answer is to ask once rather than to ask less often:
//   struct PositionSnapshot { Quantity position; std::optional<Price> avgEntryPrice; };
//   virtual PositionSnapshot IPositionManager::snapshot(SymbolId) const;
//   -- one call, one lock, one pass over the lots -- with refreshPosition()
//   calling it instead of the two getters.
TEST(StrategyPositionRefresh, ATickAsksThePositionManagerAtMostOnce)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  CountingPositionManager pm;
  CountingStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.setPositionManager(&pm);
  pm.setFill(Quantity::fromDouble(0.5), Price::fromDouble(100.0));

  constexpr int kTicks = 1000;
  pm.queries = 0;
  for (int i = 0; i < kTicks; ++i)
  {
    strat.onTrade(makeTrade(sym, 100.0 + i * 0.01, i + 1));
  }

  EXPECT_LE(pm.queries, kTicks)
      << kTicks << " ticks cost " << pm.queries
      << " calls into the position manager: getPosition() and "
         "getAverageEntryPrice() are asked separately, so every market-data "
         "tick takes the tracker's mutex twice and walks the lot deque twice, "
         "on the thread the execution path contends with";
}
