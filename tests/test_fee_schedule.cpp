/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/fee_schedule.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
constexpr int64_t SEC = 1'000'000'000LL;
constexpr int64_t DAY = 24LL * 3600LL * SEC;
}  // namespace

TEST(FeeSchedule, EmptyScheduleHasZeroFee)
{
  FeeSchedule s;
  EXPECT_EQ(s.feeFor(0, 1000.0, true), 0.0);
  EXPECT_EQ(s.feeFor(0, 1000.0, false), 0.0);
}

TEST(FeeSchedule, SingleTierMakerTaker)
{
  FeeSchedule s;
  s.addTier(0, 2.0, 4.0);

  // 1000 USDT notional, taker @ 4 bps = 0.4 USDT fee.
  EXPECT_NEAR(s.feeFor(0, 1000.0, false), 0.4, 1e-9);
  // Maker @ 2 bps = 0.2 USDT.
  EXPECT_NEAR(s.feeFor(0, 1000.0, true), 0.2, 1e-9);
}

TEST(FeeSchedule, NegativeMakerRateRebate)
{
  FeeSchedule s;
  s.addTier(0, -0.5, 1.7);
  // Maker rebate: -0.5 bps on 10000 = -0.5 USDT (received).
  EXPECT_NEAR(s.feeFor(0, 10000.0, true), -0.5, 1e-9);
}

TEST(FeeSchedule, RollingNotionalAdvancesTier)
{
  FeeSchedule s;
  s.addTier(0, 2.0, 4.0);
  s.addTier(1'000'000, 1.0, 3.0);

  // Sub-tier 1: 100k notional, still tier 0.
  s.recordFill(SEC, 100'000.0);
  EXPECT_EQ(s.currentTierIndex(), 0u);
  EXPECT_NEAR(s.feeFor(SEC, 1000.0, false), 0.4, 1e-9);

  // Push past 1M: 100k + 900k.
  s.recordFill(2 * SEC, 900'000.0);
  EXPECT_EQ(s.currentTierIndex(), 1u);
  EXPECT_NEAR(s.feeFor(2 * SEC, 1000.0, false), 0.3, 1e-9);
  EXPECT_NEAR(s.feeFor(2 * SEC, 1000.0, true), 0.1, 1e-9);
}

TEST(FeeSchedule, TierTransitionRecorded)
{
  FeeSchedule s;
  s.addTier(0, 2.0, 4.0);
  s.addTier(1'000'000, 1.0, 3.0);

  s.recordFill(SEC, 100'000.0);
  s.recordFill(2 * SEC, 900'000.0);
  s.recordFill(3 * SEC, 100'000.0);  // stays in tier 1

  const auto& transitions = s.tierTransitionTsNs();
  ASSERT_EQ(transitions.size(), 1u);
  EXPECT_EQ(transitions[0], 2 * SEC);
}

TEST(FeeSchedule, RollingWindowEvictsAfter30Days)
{
  FeeSchedule s;
  s.addTier(0, 2.0, 4.0);
  s.addTier(1'000'000, 1.0, 3.0);

  // Crank to tier 1.
  s.recordFill(SEC, 1'200'000.0);
  EXPECT_EQ(s.currentTierIndex(), 1u);

  // Wait 31 days, record a tiny fill. The big notional has aged out;
  // 30-day total drops below the 1M cutoff.
  s.recordFill(SEC + 31 * DAY, 1'000.0);
  EXPECT_EQ(s.currentTierIndex(), 0u);
}

TEST(FeeSchedule, CannedBinanceProfile)
{
  auto s = FeeSchedule::binance_um_futures();
  EXPECT_GE(s.tierCount(), 5u);
  // Regular tier (no volume): 2 / 4 bps.
  auto [maker, taker] = s.currentBps(0);
  EXPECT_NEAR(maker, 2.0, 1e-9);
  EXPECT_NEAR(taker, 4.0, 1e-9);
}

TEST(FeeSchedule, CannedDeribitHasMakerRebateAboveLV1)
{
  auto s = FeeSchedule::deribit();
  s.recordFill(SEC, 2'000'000.0);
  auto [maker, taker] = s.currentBps(SEC);
  EXPECT_LT(maker, 0.0);  // rebate
}

TEST(FeeSchedule, MultiplePromotionsAreTracked)
{
  FeeSchedule s;
  s.addTier(0, 3.0, 5.0);
  s.addTier(100'000, 2.0, 4.0);
  s.addTier(1'000'000, 1.0, 3.0);

  s.recordFill(1 * SEC, 50'000.0);   // tier 0
  s.recordFill(2 * SEC, 60'000.0);   // -> tier 1 (110k total)
  s.recordFill(3 * SEC, 900'000.0);  // -> tier 2 (1.01M)
  EXPECT_EQ(s.currentTierIndex(), 2u);
  EXPECT_EQ(s.tierTransitionTsNs().size(), 2u);
}

TEST(FeeSchedule, ResetClearsRollingState)
{
  FeeSchedule s;
  s.addTier(0, 2.0, 4.0);
  s.addTier(1'000'000, 1.0, 3.0);
  s.recordFill(SEC, 1'200'000.0);
  EXPECT_EQ(s.currentTierIndex(), 1u);

  s.resetRolling();
  EXPECT_EQ(s.currentTierIndex(), 0u);
  EXPECT_EQ(s.rollingNotional30d(), 0.0);
  EXPECT_EQ(s.tierTransitionTsNs().size(), 0u);
}
