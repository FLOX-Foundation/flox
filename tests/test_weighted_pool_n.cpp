/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/amm_pricing.h"
#include "flox/backtest/weighted_curve.h"
#include "flox/backtest/weighted_pool_n.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

// A 2-asset WeightedPoolN must reproduce the 2-token WeightedCurve exactly:
// token 0 is base, token 1 is quote.
TEST(WeightedPoolNTest, TwoAssetMatchesWeightedCurve)
{
  WeightedPoolN pool({100.0, 100.0}, {0.8, 0.2}, 30);
  WeightedCurve w(Quantity::fromDouble(100.0), Quantity::fromDouble(100.0), 0.8, 0.2, 30);

  EXPECT_NEAR(pool.spotPrice(1, 0).toDouble(), w.spotPrice().toDouble(), 1e-9);
  EXPECT_NEAR(pool.amountOut(0, 1, Quantity::fromDouble(10.0)).toDouble(),
              w.amountOut(Quantity::fromDouble(10.0), true).toDouble(), 1e-6);
}

// Equal weights reduce a pair to constant-product.
TEST(WeightedPoolNTest, EqualWeightsAreConstantProductOnAPair)
{
  const double third = 1.0 / 3.0;
  WeightedPoolN pool({1000.0, 1000.0, 1000.0}, {third, third, third}, 0);
  ConstantProductCurve cp(Quantity::fromDouble(1000.0), Quantity::fromDouble(1000.0), 0);
  EXPECT_NEAR(pool.amountOut(0, 1, Quantity::fromDouble(100.0)).toDouble(),
              cp.amountOut(Quantity::fromDouble(100.0), true).toDouble(), 1e-6);
}

// A 3-asset pool prices every pair from the balances and weights, and the
// pricing is reciprocal across the pair.
TEST(WeightedPoolNTest, ThreeAssetPairPricing)
{
  WeightedPoolN pool({100.0, 200.0, 400.0}, {0.5, 0.3, 0.2}, 0);
  EXPECT_EQ(pool.tokenCount(), 3u);

  // spot(0,1) = (B0/w0)/(B1/w1) = (100/0.5)/(200/0.3) = 200/666.667 = 0.3.
  EXPECT_NEAR(pool.spotPrice(0, 1).toDouble(), 0.3, 1e-9);
  // Reciprocal across the pair.
  EXPECT_NEAR(pool.spotPrice(0, 2).toDouble() * pool.spotPrice(2, 0).toDouble(), 1.0, 1e-9);
}

TEST(WeightedPoolNTest, PriceImpactGrowsWithSize)
{
  WeightedPoolN pool({1000.0, 1000.0, 1000.0}, {0.5, 0.3, 0.2}, 0);
  const double small = pool.priceImpact(0, 1, Quantity::fromDouble(1.0));
  const double big = pool.priceImpact(0, 1, Quantity::fromDouble(200.0));
  EXPECT_GE(small, 0.0);
  EXPECT_GT(big, small);
}

TEST(WeightedPoolNTest, CloneIsIndependent)
{
  WeightedPoolN pool({1000.0, 1000.0, 1000.0}, {0.5, 0.3, 0.2}, 0);
  const double spotBefore = pool.spotPrice(0, 1).toDouble();
  auto copy = pool.clone();
  copy->applySwap(0, 1, Quantity::fromDouble(200.0));
  EXPECT_NEAR(pool.spotPrice(0, 1).toDouble(), spotBefore, 1e-9);
  EXPECT_NE(copy->spotPrice(0, 1).toDouble(), spotBefore);
}

}  // namespace
