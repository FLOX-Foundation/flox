/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/weighted_curve.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

// All-18-decimal pools, so the scaling factors are 1e18 (no upscaling). Expected
// outputs come from Balancer's WeightedMath / LogExpMath integer reference,
// itself cross-checked against high-precision math to ~1e-18.
const std::vector<u256> kSf18{u256::pow10(18), u256::pow10(18)};

u256 e(const char* dec) { return u256::fromDec(dec); }

// 80/20 pool: WETH(20%) -> BAL(80%) uses exponent 0.25 (the LogExpMath path);
// BAL(80%) -> WETH(20%) uses exponent 4.0 (the fast path).
TEST(WeightedCurveTest, EightyTwentyToTheWei)
{
  WeightedCurve pool({e("1000000000000000000000000"), e("1000000000000000000000")}, kSf18,
                     {e("800000000000000000"), e("200000000000000000")}, e("10000000000000000"));
  EXPECT_EQ(pool.amountOut(1, 0, e("5000000000000000000")).toDec(), "1233685640804376000000");
  EXPECT_EQ(pool.amountOut(0, 1, e("10000000000000000000000")).toDec(), "38638975018959897000");
}

// 50/50 pool: exponent 1.0, the simplest fast path.
TEST(WeightedCurveTest, FiftyFiftyToTheWei)
{
  WeightedCurve pool({e("2000000000000000000000000"), e("5000000000000000000000")}, kSf18,
                     {e("500000000000000000"), e("500000000000000000")}, e("3000000000000000"));
  EXPECT_EQ(pool.amountOut(0, 1, e("1000000000000000000000")).toDec(), "2491258107833245000");
  EXPECT_EQ(pool.amountOut(1, 0, e("2000000000000000000")).toDec(), "797282043920884000000");
}

// 60/40 pool: exponents 1.5 and 0.666..., both the LogExpMath path.
TEST(WeightedCurveTest, SixtyFortyLogExpPath)
{
  WeightedCurve pool({e("600000000000000000000000"), e("400000000000000000000000")}, kSf18,
                     {e("600000000000000000"), e("400000000000000000")}, e("5000000000000000"));
  EXPECT_EQ(pool.amountOut(0, 1, e("1000000000000000000000")).toDec(), "992941430946869200000");
  EXPECT_EQ(pool.amountOut(1, 0, e("1000000000000000000000")).toDec(), "992941998067627200000");
}

TEST(WeightedCurveTest, ApplySwapAndClone)
{
  WeightedCurve pool({e("2000000000000000000000000"), e("5000000000000000000000")}, kSf18,
                     {e("500000000000000000"), e("500000000000000000")}, e("3000000000000000"));
  EXPECT_EQ(pool.tokenCount(), 2u);
  const u256 dx = e("1000000000000000000000");
  const u256 out = pool.amountOut(0, 1, dx);
  auto copy = pool.clone();
  EXPECT_EQ(pool.applySwap(0, 1, dx).toDec(), out.toDec());
  EXPECT_EQ(pool.balances()[0].toDec(), "2001000000000000000000000");   // moved
  EXPECT_EQ(copy->balances()[0].toDec(), "2000000000000000000000000");  // clone intact
}

}  // namespace
