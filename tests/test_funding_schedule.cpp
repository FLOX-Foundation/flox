/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/funding_schedule.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;
constexpr SymbolId ETH = 2;
constexpr int64_t HOUR = 3600LL * 1'000'000'000LL;
}  // namespace

TEST(FundingSchedule, ConstantIntervalFiresAtBoundaries)
{
  auto sched = FundingSchedule::constant(8 * HOUR, 0.0001);  // 0.01% / 8h

  // Tick at 7h — no boundary crossed.
  auto p1 = sched.tick(7 * HOUR, {BTC}, {1.0}, {50000.0});
  EXPECT_TRUE(p1.empty());

  // Tick at 9h — crossed the 8h boundary once.
  auto p2 = sched.tick(9 * HOUR, {BTC}, {1.0}, {50000.0});
  ASSERT_EQ(p2.size(), 1u);
  EXPECT_EQ(p2[0].timestampNs, 8 * HOUR);
  EXPECT_EQ(p2[0].symbol, BTC);
  // long position pays positive funding: 1.0 * 50000 * 0.0001 = 5.0 paid
  EXPECT_NEAR(p2[0].amount, -5.0, 1e-9);
}

TEST(FundingSchedule, MultipleSymbolsSamePayment)
{
  auto sched = FundingSchedule::constant(8 * HOUR, 0.0002);

  auto p = sched.tick(9 * HOUR, {BTC, ETH},
                      {1.0, -2.0},         // long 1 BTC, short 2 ETH
                      {50000.0, 3000.0});  // BTC=50k, ETH=3k
  ASSERT_EQ(p.size(), 2u);
  // BTC long pays:    1 * 50000 * 0.0002 = 10 paid
  // ETH short receives: 2 * 3000 * 0.0002 = 1.2 received
  EXPECT_NEAR(p[0].amount, -10.0, 1e-9);
  EXPECT_NEAR(p[1].amount, +1.2, 1e-9);
}

TEST(FundingSchedule, NegativeRateFlipsSign)
{
  auto sched = FundingSchedule::constant(8 * HOUR, -0.0003);

  auto p = sched.tick(9 * HOUR, {BTC}, {1.0}, {50000.0});
  ASSERT_EQ(p.size(), 1u);
  // long receives in negative-funding regime: -(1 * 50000 * -0.0003) = +15
  EXPECT_NEAR(p[0].amount, +15.0, 1e-9);
}

TEST(FundingSchedule, ZeroPositionSkipped)
{
  auto sched = FundingSchedule::constant(8 * HOUR, 0.0001);

  auto p = sched.tick(9 * HOUR, {BTC, ETH}, {0.0, 1.0}, {50000.0, 3000.0});
  ASSERT_EQ(p.size(), 1u);
  EXPECT_EQ(p[0].symbol, ETH);
}

TEST(FundingSchedule, AdvanceCursorPreventsReplay)
{
  auto sched = FundingSchedule::constant(8 * HOUR, 0.0001);

  auto p1 = sched.tick(9 * HOUR, {BTC}, {1.0}, {50000.0});
  ASSERT_EQ(p1.size(), 1u);

  // Tick again at the same boundary — no replay.
  auto p2 = sched.tick(9 * HOUR, {BTC}, {1.0}, {50000.0});
  EXPECT_TRUE(p2.empty());

  // Next boundary at 16h.
  auto p3 = sched.tick(17 * HOUR, {BTC}, {1.0}, {50000.0});
  ASSERT_EQ(p3.size(), 1u);
  EXPECT_EQ(p3[0].timestampNs, 16 * HOUR);
}

TEST(FundingSchedule, MultipleBoundariesInSingleTick)
{
  auto sched = FundingSchedule::constant(8 * HOUR, 0.0001);
  // Advance through 24h — three 8h boundaries.
  sched.tick(0, {BTC}, {1.0}, {50000.0});
  auto p = sched.tick(25 * HOUR, {BTC}, {1.0}, {50000.0});
  EXPECT_EQ(p.size(), 3u);  // boundaries at 8h, 16h, 24h
}

TEST(FundingSchedule, TapeModeUsesRecordedRates)
{
  auto sched = FundingSchedule::tape({
      {8 * HOUR, 0.0001},
      {16 * HOUR, -0.00005},
      {24 * HOUR, 0.0002},
  });

  auto p = sched.tick(20 * HOUR, {BTC}, {1.0}, {50000.0});
  ASSERT_EQ(p.size(), 2u);
  // 8h: long pays 1*50000*0.0001 = 5 → -5
  // 16h: long receives -(1*50000*-0.00005) = +2.5
  EXPECT_NEAR(p[0].amount, -5.0, 1e-9);
  EXPECT_NEAR(p[1].amount, +2.5, 1e-9);
}

TEST(FundingSchedule, ResetClearsCursor)
{
  auto sched = FundingSchedule::constant(8 * HOUR, 0.0001);
  sched.tick(9 * HOUR, {BTC}, {1.0}, {50000.0});
  sched.reset();
  EXPECT_EQ(sched.lastTickNs(), 0);
  // Same window now re-emits.
  auto p = sched.tick(9 * HOUR, {BTC}, {1.0}, {50000.0});
  EXPECT_EQ(p.size(), 1u);
}

TEST(FundingSchedule, CannedProfilesHaveCorrectIntervals)
{
  EXPECT_EQ(FundingSchedule::binance_um_futures().intervalNs(), 8 * HOUR);
  EXPECT_EQ(FundingSchedule::bybit_linear().intervalNs(), 8 * HOUR);
  EXPECT_EQ(FundingSchedule::okx_swap().intervalNs(), 8 * HOUR);
  EXPECT_EQ(FundingSchedule::bitget_hourly().intervalNs(), HOUR);
}

// === T047: per-symbol tape ===

TEST(FundingSchedule, TapeBySymbolPicksRatePerSymbolPerSettlement)
{
  // Two symbols, two settlements, four (sym, ts) rates.
  std::vector<FundingTapeEntry> entries = {
      {8 * HOUR, BTC, 0.0001},
      {8 * HOUR, ETH, -0.0002},
      {16 * HOUR, BTC, 0.0003},
      {16 * HOUR, ETH, 0.00015},
  };
  auto sched = FundingSchedule::tapeBySymbol(entries);

  auto p = sched.tick(20 * HOUR, {BTC, ETH}, {1.0, 2.0}, {50000.0, 3000.0});
  ASSERT_EQ(p.size(), 4u);
  // Order is by ts asc; within a ts, the order matches the symbols
  // vector. So: BTC@8h, ETH@8h, BTC@16h, ETH@16h.
  EXPECT_EQ(p[0].symbol, BTC);
  EXPECT_NEAR(p[0].rate, 0.0001, 1e-12);
  EXPECT_EQ(p[1].symbol, ETH);
  EXPECT_NEAR(p[1].rate, -0.0002, 1e-12);
  EXPECT_EQ(p[2].symbol, BTC);
  EXPECT_NEAR(p[2].rate, 0.0003, 1e-12);
  EXPECT_EQ(p[3].symbol, ETH);
  EXPECT_NEAR(p[3].rate, 0.00015, 1e-12);
}

TEST(FundingSchedule, TapeBySymbolFallsBackToConstantWhenSymbolMissing)
{
  // BTC has both settlements; ETH only the second. The first
  // settlement uses the constant-rate fallback for ETH (which we
  // override to a non-zero value to make the difference visible).
  std::vector<FundingTapeEntry> entries = {
      {8 * HOUR, BTC, 0.0001},
      {16 * HOUR, BTC, 0.0003},
      {16 * HOUR, ETH, 0.00015},
  };
  auto sched = FundingSchedule::tapeBySymbol(entries);
  sched.setConstantRate(0.00005);  // fallback for missing (sym, ts)

  auto p = sched.tick(20 * HOUR, {BTC, ETH}, {1.0, 2.0}, {50000.0, 3000.0});
  ASSERT_EQ(p.size(), 4u);
  // ETH at 8h has no entry → falls back to constant rate.
  EXPECT_EQ(p[1].symbol, ETH);
  EXPECT_NEAR(p[1].rate, 0.00005, 1e-12);
  // BTC at 8h uses tape rate.
  EXPECT_NEAR(p[0].rate, 0.0001, 1e-12);
}

TEST(FundingSchedule, EmptyTapeProducesNoPayments)
{
  auto sched = FundingSchedule::tapeBySymbol({});
  auto p = sched.tick(20 * HOUR, {BTC, ETH}, {1.0, 1.0}, {50000.0, 3000.0});
  EXPECT_TRUE(p.empty());
}

TEST(FundingSchedule, LoadTapeReadsCsvWithHeader)
{
  // Build a CSV in a temp file, load it, then run a tick.
  const std::string path = ::testing::TempDir() + "/funding_t047.csv";
  {
    std::ofstream out(path);
    out << "timestamp_ns,symbol,funding_rate\n";
    out << (8 * HOUR) << "," << static_cast<int>(BTC) << ",0.0001\n";
    out << (8 * HOUR) << "," << static_cast<int>(ETH) << ",-0.0002\n";
    out << "# a comment row should be ignored\n";
    out << (16 * HOUR) << "," << static_cast<int>(BTC) << ",0.0003\n";
  }
  FundingSchedule sched;
  ASSERT_TRUE(sched.loadTape(path));
  EXPECT_FALSE(sched.isInterval());
  ASSERT_EQ(sched.perSymbolTape().size(), 3u);
  EXPECT_EQ(sched.settlementTimestamps().size(), 2u);

  auto p = sched.tick(20 * HOUR, {BTC, ETH}, {1.0, 1.0}, {50000.0, 3000.0});
  // 8h: both symbols. 16h: BTC (entry) + ETH (constant fallback = 0).
  // ETH at 16h with rate 0 → amount 0; payment still emitted.
  ASSERT_EQ(p.size(), 4u);
  EXPECT_EQ(p[0].symbol, BTC);
  EXPECT_NEAR(p[0].rate, 0.0001, 1e-12);
  EXPECT_EQ(p[2].symbol, BTC);
  EXPECT_NEAR(p[2].rate, 0.0003, 1e-12);
  EXPECT_EQ(p[3].symbol, ETH);
  EXPECT_NEAR(p[3].rate, 0.0, 1e-12);  // fallback
}

TEST(FundingSchedule, LoadTapeReturnsFalseForMissingFile)
{
  FundingSchedule sched;
  EXPECT_FALSE(sched.loadTape("/definitely/not/here/funding.csv"));
}

TEST(FundingSchedule, LegacyTapeStillWorksAsWildcard)
{
  // Existing tape(events) API treats each (ts, rate) as a wildcard
  // entry; every symbol gets the same rate at that ts.
  auto sched = FundingSchedule::tape({
      {8 * HOUR, 0.0001},
      {16 * HOUR, 0.0002},
  });

  auto p = sched.tick(20 * HOUR, {BTC, ETH}, {1.0, 1.0}, {50000.0, 3000.0});
  ASSERT_EQ(p.size(), 4u);
  EXPECT_NEAR(p[0].rate, 0.0001, 1e-12);
  EXPECT_NEAR(p[1].rate, 0.0001, 1e-12);
  EXPECT_NEAR(p[2].rate, 0.0002, 1e-12);
  EXPECT_NEAR(p[3].rate, 0.0002, 1e-12);
}
