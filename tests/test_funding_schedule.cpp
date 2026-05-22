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
