/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Regression test for the BacktestRunner ↔ Strategy position-manager
// wire. Two independent agent sessions reported `ctx.position` and
// `ctx.is_long()` returning false-zero in `Strategy::onSymbolBar`
// after a market_buy, producing 0-trade backtests with no error.
// Root cause: `Strategy::_positionManager` was never attached in the
// backtest path, so `position()` always returned `Quantity{}` and
// `SymbolContext.position` (which the C ABI bridge reads from) was
// dead-initialised to zero. Fix: BacktestRunner registers a
// MultiModePositionTracker as an execution listener and attaches it
// to the strategy in setStrategy(); Strategy refreshes
// `c.position` from the manager before each handler call.

#include "flox/aggregator/events/bar_event.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace flox;

namespace
{

class TrackingStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  struct Sample
  {
    size_t idx;
    double position;
    bool is_long;
    bool is_flat;
  };

  std::vector<Sample> samples;
  size_t bar_count{0};
  SymbolId target{0};

 protected:
  void onSymbolBar(SymbolContext& ctx, const BarEvent& /*ev*/) override
  {
    if (bar_count == 1)
    {
      emitMarketBuy(target, Quantity::fromDouble(0.5));
    }
    samples.push_back(Sample{
        .idx = bar_count,
        .position = ctx.position.toDouble(),
        .is_long = ctx.position > Quantity{},
        .is_flat = ctx.position.isZero(),
    });
    ++bar_count;
  }
};

BarEvent makeBar(SymbolId sym, double close, int64_t ts_ns)
{
  BarEvent ev{};
  ev.symbol = sym;
  ev.barType = BarType::Time;
  ev.barTypeParam = 60'000'000'000ull;
  ev.bar.open = Price::fromDouble(close);
  ev.bar.high = Price::fromDouble(close + 0.5);
  ev.bar.low = Price::fromDouble(close - 0.5);
  ev.bar.close = Price::fromDouble(close);
  ev.bar.volume = Volume::fromDouble(100.0);
  ev.bar.startTime = TimePoint{std::chrono::nanoseconds{ts_ns}};
  ev.bar.endTime = TimePoint{std::chrono::nanoseconds{ts_ns + 60'000'000'000LL}};
  ev.bar.reason = BarCloseReason::Threshold;
  return ev;
}

SymbolId addSymbol(SymbolRegistry& reg, const std::string& spelling)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = spelling;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

}  // namespace

TEST(BacktestCtxPosition, BarHandlerSeesFillAfterMarketBuy)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");

  TrackingStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;

  BacktestRunner runner;
  runner.setStrategy(&strat);

  std::vector<BarEvent> bars;
  for (int i = 0; i < 5; ++i)
  {
    bars.push_back(makeBar(sym, 100.0 + i, i * 60'000'000'000LL));
  }
  runner.runBars(bars);

  ASSERT_EQ(strat.samples.size(), 5u);
  // Bar 0: nothing emitted yet → flat.
  EXPECT_TRUE(strat.samples[0].is_flat);
  EXPECT_FALSE(strat.samples[0].is_long);
  EXPECT_DOUBLE_EQ(strat.samples[0].position, 0.0);
  // Bar 1: ctx is snapshotted at the start of the handler (Strategy
  // refreshes c.position from the position manager before calling
  // onSymbolBar). The market_buy emitted *during* this handler lands
  // on the position manager but the local ctx reference is already a
  // copy — sample for bar 1 still reports flat. The fill is visible
  // on the next handler call, when refreshPosition pulls in the new
  // value.
  EXPECT_TRUE(strat.samples[1].is_flat)
      << "ctx is snapshotted at start of handler; fills land for the next call";
  // Bars 2–4: refreshPosition picks up the fill from bar 1.
  for (size_t i = 2; i < 5; ++i)
  {
    EXPECT_TRUE(strat.samples[i].is_long)
        << "ctx.is_long() should be true on bar " << i << " (fill landed on bar 1)";
    EXPECT_FALSE(strat.samples[i].is_flat);
    EXPECT_DOUBLE_EQ(strat.samples[i].position, 0.5);
  }
}

// Without the fix, the test above passes vacuously because bar 1 is
// already flat. The crucial assertion is that bars 2-4 are long. This
// negative case pins what the bug looked like before: a strategy that
// only buys when flat keeps buying every bar because is_flat() never
// flips to false.
TEST(BacktestCtxPosition, FlatGuardedEntryFiresOncePerBuyCycle)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");

  struct GuardedStrategy : public Strategy
  {
    using Strategy::Strategy;
    SymbolId target{0};
    int buys{0};

   protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& /*ev*/) override
    {
      if (ctx.isFlat())
      {
        emitMarketBuy(target, Quantity::fromDouble(0.5));
        ++buys;
      }
    }
  };

  GuardedStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;

  BacktestRunner runner;
  runner.setStrategy(&strat);

  std::vector<BarEvent> bars;
  for (int i = 0; i < 10; ++i)
  {
    bars.push_back(makeBar(sym, 100.0 + i, i * 60'000'000'000LL));
  }
  runner.runBars(bars);

  // With the fix: bar 0 is flat → buy. From bar 1 onwards ctx reports
  // long (fill from bar 0 landed via the position tracker), so the
  // is_flat() guard prevents further buys. Exactly one buy.
  // Without the fix: ctx is always flat → 10 buys.
  EXPECT_EQ(strat.buys, 1)
      << "is_flat() guard should prevent re-entry once a position is open";
}
