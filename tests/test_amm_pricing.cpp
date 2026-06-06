/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/amm_pricing.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

AmmPool pool(double base, double quote, int32_t feeBps)
{
  return AmmPool(Quantity::fromDouble(base), Quantity::fromDouble(quote), feeBps);
}

TEST(AmmPricingTest, SpotPriceIsQuoteOverBase)
{
  auto p = pool(1000.0, 2000.0, 0);
  EXPECT_NEAR(p.spotPrice().toDouble(), 2.0, 1e-9);
}

// Known swap against known reserves: 1000 base into a 1000/1000 pool with no
// fee yields 1000*1000/(1000+1000) = 500 quote.
TEST(AmmPricingTest, AmountOutKnownValue)
{
  auto p = pool(1000.0, 1000.0, 0);
  EXPECT_NEAR(p.amountOut(Quantity::fromDouble(1000.0), true).toDouble(), 500.0, 1e-6);
}

TEST(AmmPricingTest, FeeReducesOutput)
{
  auto noFee = pool(1000.0, 1000.0, 0);
  auto withFee = pool(1000.0, 1000.0, 30);  // 0.30%
  double a = noFee.amountOut(Quantity::fromDouble(1000.0), true).toDouble();
  double b = withFee.amountOut(Quantity::fromDouble(1000.0), true).toDouble();
  EXPECT_LT(b, a);
  EXPECT_NEAR(b, 1000.0 * 997.0 / (1000.0 + 997.0), 1e-6);
}

TEST(AmmPricingTest, PriceImpactGrowsWithSize)
{
  auto p = pool(1000.0, 1000.0, 0);
  double small = p.priceImpact(Quantity::fromDouble(1.0), true);
  double big = p.priceImpact(Quantity::fromDouble(500.0), true);
  EXPECT_GE(small, 0.0);
  EXPECT_LT(small, 0.01);
  EXPECT_GT(big, small);
}

// A swap moves reserves along the curve; with no fee, k = base * quote is
// preserved.
TEST(AmmPricingTest, ApplySwapPreservesK)
{
  auto p = pool(1000.0, 1000.0, 0);
  double kBefore = p.reserveBase().toDouble() * p.reserveQuote().toDouble();
  Quantity out = p.applySwap(Quantity::fromDouble(1000.0), true);
  EXPECT_NEAR(out.toDouble(), 500.0, 1e-6);
  EXPECT_NEAR(p.reserveBase().toDouble(), 2000.0, 1e-6);
  EXPECT_NEAR(p.reserveQuote().toDouble(), 500.0, 1e-6);
  double kAfter = p.reserveBase().toDouble() * p.reserveQuote().toDouble();
  EXPECT_NEAR(kAfter, kBefore, 1.0);
}

TEST(AmmPricingTest, QuoteForBaseDirection)
{
  auto p = pool(1000.0, 1000.0, 0);
  EXPECT_NEAR(p.amountOut(Quantity::fromDouble(1000.0), false).toDouble(), 500.0, 1e-6);
}

}  // namespace
