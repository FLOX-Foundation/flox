/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

TEST(CapiBacktest, SlippageFixedBps)
{
  auto exec = flox_executor_create();
  flox_executor_set_default_slippage(exec, FLOX_SLIPPAGE_FIXED_BPS, 0, 100.0, 0.0);
  flox_executor_on_best_levels(exec, 1, 100.0, 10.0, 100.0, 10.0);
  flox_executor_submit_order(exec, /*id=*/1, /*side=buy=*/0, 0.0, 1.0, /*type=market=*/1, 1);
  ASSERT_EQ(flox_executor_fill_count(exec), 1u);
  flox_executor_destroy(exec);
}

TEST(CapiBacktest, QueueFillOnTradeAhead)
{
  auto exec = flox_executor_create();
  flox_executor_set_queue_model(exec, FLOX_QUEUE_TOB, 1);

  // Best ask at 101, no bids. Buy limit at 99 sits behind everyone else's
  // bid-side queue of size 0 (fresh level), waiting for a sell print at 99.
  flox_executor_on_best_levels(exec, 1, 99.0, 0.0, 101.0, 5.0);

  flox_executor_submit_order(exec, 42, /*buy=*/0, 99.0, 2.0, /*type=limit=*/0, 1);
  EXPECT_EQ(flox_executor_fill_count(exec), 0u);

  flox_executor_on_trade_qty(exec, 1, 99.0, 2.0, /*is_buy=*/0);
  EXPECT_EQ(flox_executor_fill_count(exec), 1u);
  flox_executor_destroy(exec);
}

TEST(CapiBacktest, ResultStatsAndEquityCurve)
{
  auto result = flox_backtest_result_create(10000.0, 0.0, 1, 0.0, 0.0, 252.0);

  flox_backtest_result_record_fill(result, 1, 1, /*buy=*/0, 100.0, 1.0, 1000);
  flox_backtest_result_record_fill(result, 2, 1, /*sell=*/1, 110.0, 1.0, 2000);
  flox_backtest_result_record_fill(result, 3, 1, 0, 100.0, 1.0, 3000);
  flox_backtest_result_record_fill(result, 4, 1, 1, 95.0, 1.0, 4000);

  FloxBacktestStats s{};
  flox_backtest_result_stats(result, &s);
  EXPECT_EQ(s.totalTrades, 2u);
  EXPECT_EQ(s.winningTrades, 1u);
  EXPECT_EQ(s.losingTrades, 1u);
  EXPECT_NEAR(s.totalPnl, 5.0, 1e-6);

  const uint32_t n = flox_backtest_result_equity_curve(result, nullptr, 0);
  EXPECT_EQ(n, 2u);
  FloxEquityPoint pts[2];
  EXPECT_EQ(flox_backtest_result_equity_curve(result, pts, 2), 2u);
  EXPECT_GT(pts[0].equity, 10000.0);

  auto path = std::filesystem::temp_directory_path() / "flox_capi_eq.csv";
  EXPECT_EQ(flox_backtest_result_write_equity_curve_csv(result, path.c_str()), 1);
  std::ifstream in(path);
  ASSERT_TRUE(in);
  std::string line;
  std::getline(in, line);
  EXPECT_EQ(line, "timestamp_ns,equity,drawdown_pct");
  std::filesystem::remove(path);

  flox_backtest_result_destroy(result);
}
