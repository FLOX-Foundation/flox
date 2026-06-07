/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/amm_pricing.h"
#include "flox/backtest/cryptoswap_curve.h"
#include "flox/backtest/cryptoswap_pool_n.h"
#include "flox/backtest/stableswap_pool_n.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

constexpr double kA = 100.0;
constexpr double kGamma = 0.1;

// A 2-coin CryptoswapPoolN with unit price scale must reproduce the 2-coin
// CryptoswapCurve: token 0 is base, token 1 is quote.
TEST(CryptoswapPoolNTest, TwoCoinMatchesCryptoswapCurve)
{
  CryptoswapPoolN pool({1'000'000.0, 1'000'000.0}, {1.0}, kA, kGamma, 0);
  CryptoswapCurve c(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), kA, kGamma,
                    0);
  EXPECT_NEAR(pool.amountOut(0, 1, Quantity::fromDouble(100'000.0)).toDouble(),
              c.amountOut(Quantity::fromDouble(100'000.0), true).toDouble(), 1.0);
}

TEST(CryptoswapPoolNTest, ThreeCoinBalancedSpot)
{
  CryptoswapPoolN pool({1'000'000.0, 1'000'000.0, 1'000'000.0}, {1.0, 1.0}, kA, kGamma, 0);
  EXPECT_EQ(pool.tokenCount(), 3u);
  EXPECT_NEAR(pool.spotPrice(0, 1).toDouble(), 1.0, 1e-2);
  EXPECT_NEAR(pool.spotPrice(1, 2).toDouble(), 1.0, 1e-2);
}

// For a pair, the cryptoswap pool sits between stableswap (flattest) and
// constant-product: flatter than constant-product near balance, less flat than
// a pure stableswap once the swap pushes the pair off balance.
TEST(CryptoswapPoolNTest, BetweenStableswapAndConstantProduct)
{
  const Quantity in = Quantity::fromDouble(400'000.0);
  CryptoswapPoolN crypto({1'000'000.0, 1'000'000.0, 1'000'000.0}, {1.0, 1.0}, kA, kGamma, 0);
  StableSwapPoolN stable({1'000'000.0, 1'000'000.0, 1'000'000.0}, kA, 0);
  ConstantProductCurve cp(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 0);

  const double cOut = crypto.amountOut(0, 1, in).toDouble();
  const double sOut = stable.amountOut(0, 1, in).toDouble();
  const double cpOut = cp.amountOut(in, true).toDouble();
  EXPECT_GT(cOut, cpOut);  // flatter than constant-product
  EXPECT_LT(cOut, sOut);   // but less flat than stableswap
}

TEST(CryptoswapPoolNTest, InvariantPreservedAcrossSwap)
{
  CryptoswapPoolN pool({1'000'000.0, 1'000'000.0, 1'000'000.0}, {1.0, 1.0}, kA, kGamma, 0);
  const double startBase = pool.balances()[0];
  const Quantity out = pool.applySwap(0, 1, Quantity::fromDouble(50'000.0));
  pool.applySwap(1, 0, out);
  EXPECT_NEAR(pool.balances()[0], startBase, 5.0);
}

// price_scale of 2 means coin 1 is worth two of coin 0, so the spot reads ~2.
TEST(CryptoswapPoolNTest, PriceScaleSetsSpot)
{
  CryptoswapPoolN pool({2'000'000.0, 1'000'000.0}, {2.0}, kA, kGamma, 0);
  EXPECT_NEAR(pool.spotPrice(0, 1).toDouble(), 2.0, 5e-2);
}

TEST(CryptoswapPoolNTest, PriceImpactGrowsWithSize)
{
  CryptoswapPoolN pool({1'000'000.0, 1'000'000.0, 1'000'000.0}, {1.0, 1.0}, kA, kGamma, 0);
  const double small = pool.priceImpact(0, 1, Quantity::fromDouble(1'000.0));
  const double big = pool.priceImpact(0, 1, Quantity::fromDouble(500'000.0));
  EXPECT_GE(small, 0.0);
  EXPECT_GT(big, small);
}

TEST(CryptoswapPoolNTest, CloneIsIndependent)
{
  CryptoswapPoolN pool({1'000'000.0, 1'000'000.0, 1'000'000.0}, {1.0, 1.0}, kA, kGamma, 0);
  const double spotBefore = pool.spotPrice(0, 1).toDouble();
  auto copy = pool.clone();
  copy->applySwap(0, 1, Quantity::fromDouble(200'000.0));
  EXPECT_NEAR(pool.spotPrice(0, 1).toDouble(), spotBefore, 1e-3);
}

}  // namespace
