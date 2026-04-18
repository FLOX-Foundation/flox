/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_result.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace flox;

namespace
{
void roundTrip(BacktestResult& result, double entry, double exit, UnixNanos enterTs,
               UnixNanos exitTs, OrderId baseId)
{
  Fill buy;
  buy.orderId = baseId;
  buy.symbol = 1;
  buy.side = Side::BUY;
  buy.price = Price::fromDouble(entry);
  buy.quantity = Quantity::fromDouble(1.0);
  buy.timestampNs = enterTs;
  result.recordFill(buy);

  Fill sell;
  sell.orderId = baseId + 1;
  sell.symbol = 1;
  sell.side = Side::SELL;
  sell.price = Price::fromDouble(exit);
  sell.quantity = Quantity::fromDouble(1.0);
  sell.timestampNs = exitTs;
  result.recordFill(sell);
}
}  // namespace

TEST(BacktestMetrics, TradeRecordPopulatedFully)
{
  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestResult result(cfg);
  roundTrip(result, 100.0, 110.0, 1000, 2000, 1);

  ASSERT_EQ(result.trades().size(), 1u);
  const auto& t = result.trades()[0];
  EXPECT_DOUBLE_EQ(t.entryPrice.toDouble(), 100.0);
  EXPECT_DOUBLE_EQ(t.exitPrice.toDouble(), 110.0);
  EXPECT_DOUBLE_EQ(t.quantity.toDouble(), 1.0);
  EXPECT_EQ(t.entryTimeNs, 1000);
  EXPECT_EQ(t.exitTimeNs, 2000);
}

TEST(BacktestMetrics, ConsecutiveStreaks)
{
  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestResult result(cfg);

  // Pattern: W W W L L W L (max wins=3, max losses=2)
  const double pairs[][2] = {
      {100, 110}, {100, 105}, {100, 102},  // 3 wins
      {100, 95},
      {100, 99},   // 2 losses
      {100, 101},  // 1 win
      {100, 95},   // 1 loss
  };
  OrderId id = 1;
  UnixNanos ts = 1000;
  for (const auto& p : pairs)
  {
    roundTrip(result, p[0], p[1], ts, ts + 100, id);
    id += 2;
    ts += 1000;
  }

  auto stats = result.computeStats();
  EXPECT_EQ(stats.maxConsecutiveWins, 3u);
  EXPECT_EQ(stats.maxConsecutiveLosses, 2u);
}

TEST(BacktestMetrics, DurationStats)
{
  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestResult result(cfg);
  roundTrip(result, 100.0, 110.0, 0, 1000, 1);
  roundTrip(result, 100.0, 95.0, 2000, 5000, 3);
  roundTrip(result, 100.0, 120.0, 6000, 6500, 5);

  auto stats = result.computeStats();
  EXPECT_NEAR(stats.maxTradeDurationNs, 3000.0, 1.0);
  EXPECT_NEAR(stats.avgTradeDurationNs, (1000 + 3000 + 500) / 3.0, 1.0);
  EXPECT_NEAR(stats.medianTradeDurationNs, 1000.0, 1.0);
}

TEST(BacktestMetrics, EquityCurveRecordedPerTrade)
{
  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestResult result(cfg);
  roundTrip(result, 100.0, 110.0, 1000, 2000, 1);
  roundTrip(result, 100.0, 95.0, 3000, 4000, 3);

  const auto& curve = result.equityCurve();
  ASSERT_EQ(curve.size(), 2u);
  EXPECT_GT(curve[0].equity, cfg.initialCapital);
  EXPECT_LT(curve[1].equity, curve[0].equity);
  EXPECT_EQ(curve[1].timestampNs, 4000u);
}

TEST(BacktestMetrics, EquityCurveCsvRoundTrip)
{
  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestResult result(cfg);
  roundTrip(result, 100.0, 110.0, 1000, 2000, 1);
  roundTrip(result, 100.0, 95.0, 3000, 4000, 3);

  auto path = std::filesystem::temp_directory_path() / "flox_eq_curve.csv";
  ASSERT_TRUE(result.writeEquityCurveCsv(path.string()));

  std::ifstream in(path);
  ASSERT_TRUE(in);
  std::string header;
  std::getline(in, header);
  EXPECT_EQ(header, "timestamp_ns,equity,drawdown_pct");

  size_t rows = 0;
  std::string line;
  while (std::getline(in, line))
  {
    if (!line.empty())
    {
      ++rows;
    }
  }
  EXPECT_EQ(rows, result.equityCurve().size());
  std::filesystem::remove(path);
}

TEST(BacktestMetrics, TimeWeightedReturnNonZero)
{
  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestResult result(cfg);
  roundTrip(result, 100.0, 110.0, 1000, 2000, 1);
  auto stats = result.computeStats();
  EXPECT_GT(stats.timeWeightedReturn, 0.0);
}

TEST(BacktestMetrics, AvgWinLossRatio)
{
  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestResult result(cfg);
  roundTrip(result, 100.0, 110.0, 1000, 2000, 1);  // +10
  roundTrip(result, 100.0, 95.0, 3000, 4000, 3);   // -5
  auto stats = result.computeStats();
  EXPECT_NEAR(stats.avgWinLossRatio, 2.0, 0.01);
}

TEST(BacktestMetrics, TradeFeeIncludesEntryAndExit)
{
  BacktestConfig cfg;
  cfg.feeRate = 0.001;  // 10 bps per fill
  cfg.usePercentageFee = true;
  BacktestResult result(cfg);

  // Entry fee = 100 * 1 * 0.001 = 0.1, exit fee = 110 * 1 * 0.001 = 0.11
  roundTrip(result, 100.0, 110.0, 1000, 2000, 1);

  ASSERT_EQ(result.trades().size(), 1u);
  const double tradeFee = result.trades()[0].fee.toDouble();
  EXPECT_NEAR(tradeFee, 0.21, 1e-4);

  auto stats = result.computeStats();
  // totalFees aggregates both legs as well.
  EXPECT_NEAR(stats.totalFees, 0.21, 1e-4);
}

TEST(BacktestMetrics, PartialCloseProratesEntryFee)
{
  BacktestConfig cfg;
  cfg.feeRate = 0.001;
  BacktestResult result(cfg);

  // Open long 2 @ 100 with 1 fill. Entry fee = 100 * 2 * 0.001 = 0.2.
  Fill buy;
  buy.orderId = 1;
  buy.symbol = 1;
  buy.side = Side::BUY;
  buy.price = Price::fromDouble(100.0);
  buy.quantity = Quantity::fromDouble(2.0);
  buy.timestampNs = 1000;
  result.recordFill(buy);

  // Close 1 @ 110. Exit fee = 110 * 1 * 0.001 = 0.11.
  // Trade fee = entry_portion (0.1) + exit_portion (0.11) = 0.21.
  Fill sell1;
  sell1.orderId = 2;
  sell1.symbol = 1;
  sell1.side = Side::SELL;
  sell1.price = Price::fromDouble(110.0);
  sell1.quantity = Quantity::fromDouble(1.0);
  sell1.timestampNs = 2000;
  result.recordFill(sell1);

  ASSERT_EQ(result.trades().size(), 1u);
  EXPECT_NEAR(result.trades()[0].fee.toDouble(), 0.21, 1e-4);

  // Close remaining 1 @ 120. Entry portion residual (0.1) + exit (0.12) = 0.22.
  Fill sell2;
  sell2.orderId = 3;
  sell2.symbol = 1;
  sell2.side = Side::SELL;
  sell2.price = Price::fromDouble(120.0);
  sell2.quantity = Quantity::fromDouble(1.0);
  sell2.timestampNs = 3000;
  result.recordFill(sell2);

  ASSERT_EQ(result.trades().size(), 2u);
  EXPECT_NEAR(result.trades()[1].fee.toDouble(), 0.22, 1e-4);
}
