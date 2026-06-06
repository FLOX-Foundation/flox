/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/amm_pricing.h"
#include "flox/backtest/concentrated_liquidity_curve.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

// A swap that stays inside one liquidity range is constant-product on the
// virtual reserves x_v = L/sqrt(P), y_v = L*sqrt(P). With price 1 and L 1000
// the virtual reserves are 1000/1000, so a small swap matches a 1000/1000
// constant-product pool.
TEST(ConcentratedLiquidityTest, SingleRangeMatchesConstantProduct)
{
  std::vector<ClTick> ticks{{0.25, 1000.0}, {4.0, -1000.0}};  // range [0.25, 4]
  ConcentratedLiquidityCurve cl(1.0, 1000.0, ticks, 0);
  ConstantProductCurve cp(Quantity::fromDouble(1000.0), Quantity::fromDouble(1000.0), 0);
  EXPECT_NEAR(cl.amountOut(Quantity::fromDouble(10.0), true).toDouble(),
              cp.amountOut(Quantity::fromDouble(10.0), true).toDouble(), 1e-6);
}

TEST(ConcentratedLiquidityTest, SpotPriceIsPrice)
{
  ConcentratedLiquidityCurve cl(2.25, 1000.0, {{1.0, 1000.0}, {4.0, -1000.0}}, 0);
  EXPECT_NEAR(cl.spotPrice().toDouble(), 2.25, 1e-9);
}

// Buying base pushes price up. From price 2.25 (sqrtP 1.5), the position
// [1, 4] ends at price 4 (sqrtP 2.0): moving to that boundary takes
// L*(2.0-1.5) = 500 quote in and gives L*(2.0-1.5)/(1.5*2.0) = 166.667 base.
// Past the tick the position's liquidity is gone, so more input cannot fill.
TEST(ConcentratedLiquidityTest, CrossTickCapsAtLiquidityEnd)
{
  ConcentratedLiquidityCurve cl(2.25, 1000.0, {{1.0, 1000.0}, {4.0, -1000.0}}, 0);
  const double expected = 1000.0 * (2.0 - 1.5) / (1.5 * 2.0);  // 166.6667
  EXPECT_NEAR(cl.amountOut(Quantity::fromDouble(500.0), false).toDouble(), expected, 1e-4);
  // More than fills: liquidity runs out at the tick, output is capped.
  EXPECT_NEAR(cl.amountOut(Quantity::fromDouble(5000.0), false).toDouble(), expected, 1e-4);
}

TEST(ConcentratedLiquidityTest, ApplySwapMovesPriceToBoundary)
{
  ConcentratedLiquidityCurve cl(2.25, 1000.0, {{1.0, 1000.0}, {4.0, -1000.0}}, 0);
  cl.applySwap(Quantity::fromDouble(500.0), false);  // buy base, reach the upper tick
  EXPECT_NEAR(cl.spotPrice().toDouble(), 4.0, 1e-4);
}

// Inside a range a larger swap has more price impact, as on any curve.
TEST(ConcentratedLiquidityTest, PriceImpactGrowsWithSize)
{
  ConcentratedLiquidityCurve cl(1.0, 1000.0, {{0.01, 1000.0}, {100.0, -1000.0}}, 0);
  double small = cl.priceImpact(Quantity::fromDouble(1.0), true);
  double big = cl.priceImpact(Quantity::fromDouble(100.0), true);
  EXPECT_GE(small, 0.0);
  EXPECT_GT(big, small);
}

}  // namespace
