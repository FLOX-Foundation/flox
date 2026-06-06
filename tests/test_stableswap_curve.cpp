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

#include <gtest/gtest.h>

using namespace flox;

namespace
{

// At balance the spot price of a pegged pool is 1.
TEST(StableSwapCurveTest, BalancedSpotIsOne)
{
  StableSwapCurve s(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 100.0, 0);
  EXPECT_NEAR(s.spotPrice().toDouble(), 1.0, 1e-4);
}

// The point of amplification: near the peg a stableswap fills much flatter than
// constant-product. The same swap into an equal-balance pool comes out closer
// to one-for-one than a Uniswap-v2 pool of the same size does.
TEST(StableSwapCurveTest, FlatterThanConstantProduct)
{
  StableSwapCurve s(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 200.0, 0);
  ConstantProductCurve cp(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 0);
  const Quantity in = Quantity::fromDouble(100'000.0);
  const double ssOut = s.amountOut(in, true).toDouble();
  const double cpOut = cp.amountOut(in, true).toDouble();
  EXPECT_GT(ssOut, cpOut);                 // less slippage on the stable curve
  EXPECT_LT(ssOut, in.toDouble());         // still below one-for-one
  EXPECT_GT(ssOut, 0.99 * in.toDouble());  // but close to it near the peg
}

// The invariant D is unchanged by a no-fee swap: it only redistributes balances
// along the same curve.
TEST(StableSwapCurveTest, InvariantPreservedAcrossSwap)
{
  StableSwapCurve s(Quantity::fromDouble(500'000.0), Quantity::fromDouble(500'000.0), 100.0, 0);
  // D is private; check it indirectly through a round trip. Swapping out and
  // back with no fee returns (within rounding) to the starting balances.
  const double startBase = s.reserveBase().toDouble();
  const Quantity out = s.applySwap(Quantity::fromDouble(50'000.0), true);  // base in, quote out
  s.applySwap(out, false);                                                 // quote back in
  EXPECT_NEAR(s.reserveBase().toDouble(), startBase, 1.0);
}

TEST(StableSwapCurveTest, ApplySwapMovesBalances)
{
  StableSwapCurve s(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 100.0, 0);
  const Quantity out = s.applySwap(Quantity::fromDouble(100'000.0), true);
  EXPECT_NEAR(s.reserveBase().toDouble(), 1'100'000.0, 1e-3);
  EXPECT_NEAR(s.reserveQuote().toDouble(), 1'000'000.0 - out.toDouble(), 1e-3);
}

TEST(StableSwapCurveTest, PriceImpactGrowsWithSize)
{
  StableSwapCurve s(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 100.0, 0);
  const double small = s.priceImpact(Quantity::fromDouble(1'000.0), true);
  const double big = s.priceImpact(Quantity::fromDouble(500'000.0), true);
  EXPECT_GE(small, 0.0);
  EXPECT_GT(big, small);
}

// A lower amplification curves earlier: the same swap slips more at A=10 than
// at A=1000.
TEST(StableSwapCurveTest, LowerAmpSlipsMore)
{
  const Quantity in = Quantity::fromDouble(200'000.0);
  StableSwapCurve lo(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 10.0, 0);
  StableSwapCurve hi(Quantity::fromDouble(1'000'000.0), Quantity::fromDouble(1'000'000.0), 1000.0, 0);
  EXPECT_LT(lo.amountOut(in, true).toDouble(), hi.amountOut(in, true).toDouble());
}

}  // namespace
