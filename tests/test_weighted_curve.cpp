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

#include <gtest/gtest.h>

using namespace flox;

namespace
{

// Equal weights reduce a weighted pool to constant-product.
TEST(WeightedCurveTest, FiftyFiftyMatchesConstantProduct)
{
  WeightedCurve w(Quantity::fromDouble(1000.0), Quantity::fromDouble(1000.0), 0.5, 0.5, 0);
  ConstantProductCurve cp(Quantity::fromDouble(1000.0), Quantity::fromDouble(1000.0), 0);
  EXPECT_NEAR(w.spotPrice().toDouble(), cp.spotPrice().toDouble(), 1e-9);
  EXPECT_NEAR(w.amountOut(Quantity::fromDouble(1000.0), true).toDouble(),
              cp.amountOut(Quantity::fromDouble(1000.0), true).toDouble(), 1e-6);
}

// A higher weight raises the spot price for the same balance:
// spot = (B_quote/w_quote)/(B_base/w_base) = (100/0.2)/(100/0.8) = 4.
TEST(WeightedCurveTest, WeightRaisesSpotPrice)
{
  WeightedCurve w(Quantity::fromDouble(100.0), Quantity::fromDouble(100.0), 0.8, 0.2, 0);
  EXPECT_NEAR(w.spotPrice().toDouble(), 4.0, 1e-9);
}

// Known 80/20 swap: 10 base into a 100/100 pool, no fee.
// out = 100*(1 - (100/110)^(0.8/0.2)) = 100*(1 - 0.90909^4) ~= 31.6987.
TEST(WeightedCurveTest, AmountOutKnownValue80_20)
{
  WeightedCurve w(Quantity::fromDouble(100.0), Quantity::fromDouble(100.0), 0.8, 0.2, 0);
  const double expected = 100.0 * (1.0 - std::pow(100.0 / 110.0, 0.8 / 0.2));
  EXPECT_NEAR(w.amountOut(Quantity::fromDouble(10.0), true).toDouble(), expected, 1e-6);
  EXPECT_NEAR(expected, 31.6987, 1e-3);
}

TEST(WeightedCurveTest, ApplySwapMovesReserves)
{
  WeightedCurve w(Quantity::fromDouble(100.0), Quantity::fromDouble(100.0), 0.8, 0.2, 0);
  Quantity out = w.applySwap(Quantity::fromDouble(10.0), true);
  EXPECT_NEAR(w.reserveBase().toDouble(), 110.0, 1e-6);
  EXPECT_NEAR(w.reserveQuote().toDouble(), 100.0 - out.toDouble(), 1e-6);
}

TEST(WeightedCurveTest, PriceImpactGrowsWithSize)
{
  WeightedCurve w(Quantity::fromDouble(1000.0), Quantity::fromDouble(1000.0), 0.8, 0.2, 0);
  double small = w.priceImpact(Quantity::fromDouble(1.0), true);
  double big = w.priceImpact(Quantity::fromDouble(200.0), true);
  EXPECT_GE(small, 0.0);
  EXPECT_GT(big, small);
}

}  // namespace
