/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

TEST(CapiBacktest, SlippageFixedBps)
{
  auto exec = lrvx_simulated_executor_create();
  lrvx_simulated_executor_set_default_slippage(exec, LRVX_SLIPPAGE_FIXED_BPS, 0, 0.0, 100.0, 0.0);
  lrvx_simulated_executor_on_best_levels(exec, 1, 100.0, 10.0, 100.0, 10.0);
  lrvx_simulated_executor_submit_order(exec, /*id=*/1, /*side=buy=*/0, 0.0, 1.0, /*type=market=*/1, 1);
  ASSERT_EQ(lrvx_simulated_executor_fill_count(exec), 1u);
  lrvx_simulated_executor_destroy(exec);
}

TEST(CapiBacktest, QueueFillOnTradeAhead)
{
  auto exec = lrvx_simulated_executor_create();
  lrvx_simulated_executor_set_queue_model(exec, LRVX_QUEUE_TOB, 1);

  // Best ask at 101, no bids. Buy limit at 99 sits behind everyone else's
  // bid-side queue of size 0 (fresh level), waiting for a sell print at 99.
  lrvx_simulated_executor_on_best_levels(exec, 1, 99.0, 0.0, 101.0, 5.0);

  lrvx_simulated_executor_submit_order(exec, 42, /*buy=*/0, 99.0, 2.0, /*type=limit=*/0, 1);
  EXPECT_EQ(lrvx_simulated_executor_fill_count(exec), 0u);

  lrvx_simulated_executor_on_trade_qty(exec, 1, 99.0, 2.0, /*is_buy=*/0);
  EXPECT_EQ(lrvx_simulated_executor_fill_count(exec), 1u);
  lrvx_simulated_executor_destroy(exec);
}

TEST(CapiBacktest, ResultStatsAndEquityCurve)
{
  auto result = lrvx_backtest_result_create(10000.0, 0.0, 1, 0.0, 0.0, 252.0);

  lrvx_backtest_result_record_fill(result, 1, 1, /*buy=*/0, 100.0, 1.0, 1000);
  lrvx_backtest_result_record_fill(result, 2, 1, /*sell=*/1, 110.0, 1.0, 2000);
  lrvx_backtest_result_record_fill(result, 3, 1, 0, 100.0, 1.0, 3000);
  lrvx_backtest_result_record_fill(result, 4, 1, 1, 95.0, 1.0, 4000);

  LrvxBacktestStats s{};
  lrvx_backtest_result_stats(result, &s);
  EXPECT_EQ(s.totalTrades, 2u);
  EXPECT_EQ(s.winningTrades, 1u);
  EXPECT_EQ(s.losingTrades, 1u);
  EXPECT_NEAR(s.totalPnl, 5.0, 1e-6);

  const uint32_t n = lrvx_backtest_result_equity_curve(result, nullptr, 0);
  EXPECT_EQ(n, 2u);
  LrvxEquityPoint pts[2];
  EXPECT_EQ(lrvx_backtest_result_equity_curve(result, pts, 2), 2u);
  EXPECT_GT(pts[0].equity, 10000.0);

  auto path = std::filesystem::temp_directory_path() / "lrvx_capi_eq.csv";
  EXPECT_EQ(lrvx_backtest_result_write_equity_curve_csv(result, path.c_str()), 1);
  std::ifstream in(path);
  ASSERT_TRUE(in);
  std::string line;
  std::getline(in, line);
  EXPECT_EQ(line, "timestamp_ns,equity,drawdown_pct");
  std::filesystem::remove(path);

  lrvx_backtest_result_destroy(result);
}
