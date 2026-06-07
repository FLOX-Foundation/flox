/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/concentrated_liquidity_curve.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

u256 d(const char* s) { return u256::fromDec(s); }

// A snapshot of the live USDC/WETH 0.05% pool. An in-range swap (1000 USDC ->
// WETH) stays within the active range, so no tick data is needed, and it matches
// the QuoterV2 quote to 0 wei.
TEST(ConcentratedLiquidityCurveTest, InRangeMatchesQuoterToTheWei)
{
  ConcentratedLiquidityCurve pool(d("1959100328691929984878240664321702"), d("2580696918646962643"),
                                  500, {});
  EXPECT_EQ(pool.amountOut(0, 1, d("1000000000")).toDec(), "611128907033491490");
}

// Synthetic pool to exercise tick-walking: price 1.0, one initialized tick below
// at sqrt 0.9*Q96 with liquidityNet +5e17. Vectors from the v3 swap reference.
TEST(ConcentratedLiquidityCurveTest, CrossTickWalk)
{
  const u256 q96 = d("79228162514264337593543950336");
  std::vector<ClTick> ticks{{d("71305346262837903834189555302"), i256(d("500000000000000000"))}};
  ConcentratedLiquidityCurve pool(q96, d("1000000000000000000"), 3000, ticks);

  // Small swap stays in range.
  EXPECT_EQ(pool.amountOut(0, 1, d("1000000000000000")).toDec(), "996006981039903");
  // Larger swap crosses the tick, so the second part of the swap uses the
  // reduced liquidity.
  EXPECT_EQ(pool.amountOut(0, 1, d("300000000000000000")).toDec(), "213772620630911997");
}

TEST(ConcentratedLiquidityCurveTest, ApplySwapMovesPrice)
{
  ConcentratedLiquidityCurve pool(d("1959100328691929984878240664321702"), d("2580696918646962643"),
                                  500, {});
  EXPECT_EQ(pool.tokenCount(), 2u);
  const u256 before = pool.sqrtPriceX96();
  pool.applySwap(0, 1, d("1000000000"));  // token0 in -> price down
  EXPECT_TRUE(pool.sqrtPriceX96() < before);
}

TEST(ConcentratedLiquidityCurveTest, CloneIsIndependent)
{
  ConcentratedLiquidityCurve pool(d("1959100328691929984878240664321702"), d("2580696918646962643"),
                                  500, {});
  const u256 before = pool.sqrtPriceX96();
  auto clone = pool.clone();
  clone->applySwap(0, 1, d("1000000000"));                 // mutate the clone
  EXPECT_EQ(pool.sqrtPriceX96().toDec(), before.toDec());  // original intact
}

}  // namespace
