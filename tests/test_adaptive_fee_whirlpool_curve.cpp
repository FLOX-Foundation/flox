/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/adaptive_fee_whirlpool_curve.h"

#include "flox/backtest/orca_whirlpool_curve.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{

u256 D(const char* s) { return u256::fromDec(s); }

AdaptiveFeeConstants constants(uint32_t control)
{
  AdaptiveFeeConstants c;
  c.filterPeriod = 30;
  c.decayPeriod = 600;
  c.reductionFactor = 5000;
  c.adaptiveFeeControlFactor = control;
  c.maxVolatilityAccumulator = 350000;
  c.tickGroupSize = 64;
  c.majorSwapThresholdTicks = 64;
  return c;
}

const char* SP = "18446744073709551616";  // 2^64, price 1.0, tick 0
const char* L = "500000000000000";
const uint32_t STATIC = 3000;
const uint64_t TS = 1738863309;

// The fee math against the program's own pre-calculated vector
// (test_compute_adaptive_fee_rate): volatility ramps by tick-group delta from a
// reference, and compute_adaptive_fee_rate matches to the unit.
TEST(AdaptiveFeeWhirlpoolTest, FeeRateMatchesProgramVector)
{
  const uint32_t expected[] = {
      0, 62, 246, 553, 984, 1536, 2212, 3011, 3933, 4977, 6144, 7435, 8848,
      10384, 12043, 13824, 15729, 17757, 19907, 22180, 24576, 27096, 29737, 32502, 35390, 38400,
      41534, 44790, 48169, 51672, 55296, 59044, 62915, 66909, 71025, 75264, 75264, 75264, 75264,
      75264, 75264, 75264, 75264, 75264, 75264, 75264, 75264, 75264, 75264, 75264};
  WhirlpoolFeeRateManager mgr{constants(1500)};
  AdaptiveFeeVariables v;
  mgr.updateReference(v, 16, TS);
  for (int delta = 0; delta < 50; ++delta)
  {
    mgr.updateVolatilityAccumulator(v, 16 + delta);
    EXPECT_EQ(mgr.computeAdaptiveFeeRate(v.volatilityAccumulator), expected[delta]) << "delta=" << delta;
  }
}

// With no adaptive fee (control factor 0) the program skips the tick-group
// bounding, so the swap is exactly the static OrcaWhirlpoolCurve.
TEST(AdaptiveFeeWhirlpoolTest, ControlZeroEqualsStaticWhirlpool)
{
  OrcaWhirlpoolCurve plain(D(SP), D(L), STATIC, {});
  AdaptiveFeeWhirlpoolCurve adaptive(D(SP), D(L), STATIC, {}, /*tick*/ 0, TS, constants(0),
                                     AdaptiveFeeVariables{});
  const u256 amt = D("50000000000000");
  EXPECT_EQ(adaptive.amountOut(1, 0, amt).toDec(), plain.amountOut(1, 0, amt).toDec());
  EXPECT_EQ(plain.amountOut(1, 0, amt).toDec(), "45330544694007");
}

// With the adaptive fee active, the volatility-driven fee rises as the swap crosses
// tick groups, so the output is below the static one. Matches a faithful Python
// transcription of the program's swap.
TEST(AdaptiveFeeWhirlpoolTest, AdaptiveSwapMatchesReference)
{
  AdaptiveFeeWhirlpoolCurve adaptive(D(SP), D(L), STATIC, {}, /*tick*/ 0, TS, constants(1500),
                                     AdaptiveFeeVariables{});
  const u256 amt = D("50000000000000");
  EXPECT_EQ(adaptive.amountOut(1, 0, amt).toDec(), "44616943875943");

  OrcaWhirlpoolCurve plain(D(SP), D(L), STATIC, {});
  EXPECT_TRUE(adaptive.amountOut(1, 0, amt) < plain.amountOut(1, 0, amt));  // higher fee
}

TEST(AdaptiveFeeWhirlpoolTest, ApplyAndClone)
{
  AdaptiveFeeWhirlpoolCurve pool(D(SP), D(L), STATIC, {}, 0, TS, constants(1500),
                                 AdaptiveFeeVariables{});
  const u256 amt = D("50000000000000");
  const u256 out = pool.amountOut(1, 0, amt);
  auto clone = pool.clone();
  EXPECT_EQ(pool.applySwap(1, 0, amt).toDec(), out.toDec());
  EXPECT_NE(pool.sqrtPrice().toDec(), D(SP).toDec());           // price moved up
  EXPECT_EQ(clone->amountOut(1, 0, amt).toDec(), out.toDec());  // clone untouched
}

}  // namespace
