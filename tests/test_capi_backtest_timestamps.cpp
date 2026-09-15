/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// flox_backtest_runner_run_bars() and flox_backtest_runner_run_ohlcv() take
// timestamps that are documented and named as already-nanoseconds
// (start_time_ns / end_time_ns, timestamps_ns). BacktestRunnerImpl used to
// run every one of them back through normalizeTs(), a unit-guessing helper
// that exists for runCsv()'s genuinely ambiguous input. Any nanosecond
// value under 1e12 -- the first eleven and a half days after the epoch,
// an ordinary timestamp for synthetic test data built from small offsets
// -- read as "probably seconds" and got rescaled by another 1e9: wrong in
// a release build (silent overflow), an abort under a sanitizer.
//
// No sanitizer needed to see this: the corrupted value is just wrong.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>
#include <atomic>

namespace
{

struct BarState
{
  FloxStrategyHandle strategy{nullptr};
  uint32_t symbol{0};
  int64_t last_start_ns{-1};
  int64_t last_end_ns{-1};
  int bar_count{0};
};

void on_bar(void* ud, const FloxSymbolContext*, const FloxBarData* bar)
{
  auto* st = static_cast<BarState*>(ud);
  st->last_start_ns = bar->start_time_ns;
  st->last_end_ns = bar->end_time_ns;
  st->bar_count++;
}

struct TradeState
{
  int64_t last_ts_ns{-1};
  int trade_count{0};
};

void on_trade(void* ud, const FloxSymbolContext*, const FloxTradeData* trade)
{
  auto* st = static_cast<TradeState*>(ud);
  st->last_ts_ns = trade->exchange_ts_ns;
  st->trade_count++;
}

}  // namespace

TEST(CapiBacktestTimestamps, RunBarsPassesNanosecondsThroughUnmodified)
{
  // 60 seconds since the epoch, expressed directly in nanoseconds -- an
  // ordinary value for the second bar of a synthetic test, and under the
  // 1e12 threshold normalizeTs() used to treat as "probably seconds".
  constexpr int64_t kStartNs = 60'000'000'000LL;
  constexpr int64_t kEndNs = 119'999'999'999LL;

  auto* registry = flox_registry_create();
  ASSERT_NE(registry, nullptr);
  uint32_t sym = flox_registry_add_symbol(registry, "test", "BTC", 0.01);

  BarState state;
  state.symbol = sym;

  FloxStrategyCallbacks cb{};
  cb.on_bar = on_bar;
  cb.user_data = &state;
  uint32_t syms[] = {sym};
  auto* strat = flox_strategy_create(/*id=*/1, syms, 1, registry, cb);
  ASSERT_NE(strat, nullptr);
  state.strategy = strat;

  auto* btr = flox_backtest_runner_create(registry, /*fee=*/0.0, /*initial_capital=*/10000.0);
  ASSERT_NE(btr, nullptr);
  flox_backtest_runner_set_strategy(btr, strat);

  int64_t starts[1] = {kStartNs};
  int64_t ends[1] = {kEndNs};
  double opens[1] = {100.0};
  double highs[1] = {101.0};
  double lows[1] = {99.0};
  double closes[1] = {100.5};
  double volumes[1] = {10.0};

  FloxBacktestStats stats{};
  int rc = flox_backtest_runner_run_bars(btr, starts, ends, opens, highs, lows, closes,
                                         volumes, 1, "BTC", /*bar_type=*/0,
                                         /*bar_type_param=*/0, &stats);
  EXPECT_EQ(rc, 1);
  ASSERT_EQ(state.bar_count, 1);
  EXPECT_EQ(state.last_start_ns, kStartNs)
      << "run_bars must not rescale a value that is already nanoseconds by contract";
  EXPECT_EQ(state.last_end_ns, kEndNs);

  flox_strategy_destroy(strat);
  flox_backtest_runner_destroy(btr);
  flox_registry_destroy(registry);
}

TEST(CapiBacktestTimestamps, RunOhlcvPassesNanosecondsThroughUnmodified)
{
  // Same hazard, run_ohlcv's timestamps_ns parameter: flox_backtest_
  // runner_run_ohlcv documents nanoseconds explicitly.
  constexpr int64_t kTsNs = 60'000'000'000LL;

  auto* registry = flox_registry_create();
  ASSERT_NE(registry, nullptr);
  uint32_t sym = flox_registry_add_symbol(registry, "test", "BTC", 0.01);

  TradeState state;

  FloxStrategyCallbacks cb{};
  cb.on_trade = on_trade;
  cb.user_data = &state;
  uint32_t syms[] = {sym};
  auto* strat = flox_strategy_create(/*id=*/1, syms, 1, registry, cb);
  ASSERT_NE(strat, nullptr);

  auto* btr = flox_backtest_runner_create(registry, /*fee=*/0.0, /*initial_capital=*/10000.0);
  ASSERT_NE(btr, nullptr);
  flox_backtest_runner_set_strategy(btr, strat);

  int64_t timestamps[1] = {kTsNs};
  double closes[1] = {100.5};

  FloxBacktestStats stats{};
  int rc = flox_backtest_runner_run_ohlcv(btr, timestamps, closes, 1, "BTC", &stats);
  EXPECT_EQ(rc, 1);
  ASSERT_EQ(state.trade_count, 1);
  EXPECT_EQ(state.last_ts_ns, kTsNs)
      << "run_ohlcv must not rescale a value that is already nanoseconds by contract";

  flox_strategy_destroy(strat);
  flox_backtest_runner_destroy(btr);
  flox_registry_destroy(registry);
}
