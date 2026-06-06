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
#include "flox/backtest/stableswap_curve.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

constexpr double kA = 100.0;
constexpr double kGamma = 0.1;

// At balance the spot price of an equal-reserve pool is 1.
TEST(CryptoswapCurveTest, BalancedSpotIsOne)
{
  CryptoswapCurve c(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), kA, kGamma,
                    0);
  EXPECT_NEAR(c.spotPrice().toDouble(), 1.0, 1e-3);
}

// Near balance the high K makes the curve flat, so a small swap slips less than
// constant-product, like a stableswap.
TEST(CryptoswapCurveTest, FlatterThanConstantProductNearBalance)
{
  CryptoswapCurve c(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), kA, kGamma,
                    0);
  ConstantProductCurve cp(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 0);
  const Quantity in = Quantity::fromDouble(10'000.0);
  EXPECT_GT(c.amountOut(in, true).toDouble(), cp.amountOut(in, true).toDouble());
}

// The point of cryptoswap: it sits between stableswap and constant-product. For
// a swap large enough to push the pool off balance, K decays, so the output is
// flatter than constant-product but not as flat as a pure stableswap.
TEST(CryptoswapCurveTest, BetweenStableswapAndConstantProduct)
{
  const Quantity in = Quantity::fromDouble(400'000.0);
  CryptoswapCurve c(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), kA, kGamma,
                    0);
  StableSwapCurve s(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), kA, 0);
  ConstantProductCurve cp(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 0);
  const double cOut = c.amountOut(in, true).toDouble();
  const double sOut = s.amountOut(in, true).toDouble();
  const double cpOut = cp.amountOut(in, true).toDouble();
  EXPECT_GT(cOut, cpOut);  // flatter than constant-product
  EXPECT_LT(cOut, sOut);   // but less flat than stableswap
}

// A no-fee swap only redistributes balances along the curve, so swapping out
// and back returns (within rounding) to the starting balances.
TEST(CryptoswapCurveTest, InvariantPreservedAcrossSwap)
{
  CryptoswapCurve c(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), kA, kGamma,
                    0);
  const double startBase = c.reserveBase().toDouble();
  const Quantity out = c.applySwap(Quantity::fromDouble(50'000.0), true);  // base in, quote out
  c.applySwap(out, false);                                                 // quote back in
  EXPECT_NEAR(c.reserveBase().toDouble(), startBase, 1.0);
}

TEST(CryptoswapCurveTest, ApplySwapMovesBalances)
{
  CryptoswapCurve c(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), kA, kGamma,
                    0);
  const Quantity out = c.applySwap(Quantity::fromDouble(100'000.0), true);
  EXPECT_NEAR(c.reserveBase().toDouble(), 1'100'000.0, 1e-3);
  EXPECT_NEAR(c.reserveQuote().toDouble(), 1'000'000.0 - out.toDouble(), 1e-3);
}

TEST(CryptoswapCurveTest, PriceImpactGrowsWithSize)
{
  CryptoswapCurve c(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), kA, kGamma,
                    0);
  const double small = c.priceImpact(Quantity::fromDouble(1'000.0), true);
  const double big = c.priceImpact(Quantity::fromDouble(500'000.0), true);
  EXPECT_GE(small, 0.0);
  EXPECT_GT(big, small);
}

}  // namespace
