/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/amm_pricing.h"
#include "flox/backtest/stableswap_curve.h"
#include "flox/backtest/stableswap_pool_n.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

// A 2-coin StableSwapPoolN must reproduce the 2-coin StableSwapCurve: token 0 is
// base, token 1 is quote.
TEST(StableSwapPoolNTest, TwoCoinMatchesStableSwapCurve)
{
  StableSwapPoolN pool({1'000'000.0, 1'000'000.0}, 100.0, 30);
  StableSwapCurve s(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 100.0, 30);
  EXPECT_NEAR(pool.amountOut(0, 1, Quantity::fromDouble(100'000.0)).toDouble(),
              s.amountOut(Quantity::fromDouble(100'000.0), true).toDouble(), 1.0);
}

// A balanced 3-coin pool prices each pair near 1 and fills flatter than
// constant-product on any pair.
TEST(StableSwapPoolNTest, ThreeCoinBalancedAndFlat)
{
  StableSwapPoolN pool({1'000'000.0, 1'000'000.0, 1'000'000.0}, 200.0, 0);
  EXPECT_EQ(pool.tokenCount(), 3u);
  EXPECT_NEAR(pool.spotPrice(0, 1).toDouble(), 1.0, 1e-3);
  EXPECT_NEAR(pool.spotPrice(1, 2).toDouble(), 1.0, 1e-3);

  ConstantProductCurve cp(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 0);
  const Quantity in = Quantity::fromDouble(100'000.0);
  EXPECT_GT(pool.amountOut(0, 1, in).toDouble(), cp.amountOut(in, true).toDouble());
}

// A no-fee swap only redistributes balances along the curve: out and back
// returns near the starting balances.
TEST(StableSwapPoolNTest, InvariantPreservedAcrossSwap)
{
  StableSwapPoolN pool({500'000.0, 500'000.0, 500'000.0}, 100.0, 0);
  const double startBase = pool.balances()[0];
  const Quantity out = pool.applySwap(0, 1, Quantity::fromDouble(50'000.0));
  pool.applySwap(1, 0, out);
  EXPECT_NEAR(pool.balances()[0], startBase, 1.0);
}

TEST(StableSwapPoolNTest, LowerAmpSlipsMore)
{
  const Quantity in = Quantity::fromDouble(300'000.0);
  StableSwapPoolN lo({1'000'000.0, 1'000'000.0, 1'000'000.0}, 10.0, 0);
  StableSwapPoolN hi({1'000'000.0, 1'000'000.0, 1'000'000.0}, 1000.0, 0);
  EXPECT_LT(lo.amountOut(0, 1, in).toDouble(), hi.amountOut(0, 1, in).toDouble());
}

TEST(StableSwapPoolNTest, PriceImpactGrowsWithSize)
{
  StableSwapPoolN pool({1'000'000.0, 1'000'000.0, 1'000'000.0}, 100.0, 0);
  const double small = pool.priceImpact(0, 1, Quantity::fromDouble(1'000.0));
  const double big = pool.priceImpact(0, 1, Quantity::fromDouble(500'000.0));
  EXPECT_GE(small, 0.0);
  EXPECT_GT(big, small);
}

TEST(StableSwapPoolNTest, CloneIsIndependent)
{
  StableSwapPoolN pool({1'000'000.0, 1'000'000.0, 1'000'000.0}, 100.0, 0);
  const double spotBefore = pool.spotPrice(0, 1).toDouble();
  auto copy = pool.clone();
  copy->applySwap(0, 1, Quantity::fromDouble(200'000.0));
  EXPECT_NEAR(pool.spotPrice(0, 1).toDouble(), spotBefore, 1e-6);
}

}  // namespace
