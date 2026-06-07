/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/stableswap_curve.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{

// Saber is the Curve StableSwap on Solana: the same invariant, the same Ann = A*N
// (Saber's ann = amp * N_COINS), the same y = (y^2 + c) / (2y + b - D) Newton, and
// the same dy = dest - y - 1 before the fee. It is a two-coin pool with no rate
// scaling, so it is a StableSwapCurve with identity rates (rate = PRECISION). Its
// fee is a numerator over a denominator; when the denominator divides 1e10 (Saber
// uses powers of ten) it maps exactly onto the over-1e10 fee.
//
// Expected values are from a faithful transcription of Saber's swap_to
// (stable-swap-math); StableSwapCurve reproduces them to the unit, so Saber needs
// no separate curve.
u256 D(const char* s) { return u256::fromDec(s); }

StableSwapCurve saber(u256 a, u256 b, uint64_t A, uint64_t feeNum, uint64_t feeDen)
{
  const u256 prec = u256::pow10(18);
  const u256 fee = u256(feeNum) * u256::pow10(10) / u256(feeDen);
  return StableSwapCurve({a, b}, {prec, prec}, A, fee);
}

TEST(SaberStableSwapTest, MatchesSaberSwapTo)
{
  // USDC/USDT-like (6 decimals), A=100, 0.04% fee (4/10000), swap 1000 in.
  EXPECT_EQ(saber(D("5000000000000"), D("5000000000000"), 100, 4, 10000)
                .amountOut(0, 1, D("1000000000"))
                .toDec(),
            "999598020");

  // Imbalanced reserves, a larger swap.
  EXPECT_EQ(saber(D("8000000000000"), D("3000000000000"), 100, 4, 10000)
                .amountOut(0, 1, D("500000000000"))
                .toDec(),
            "491136243886");

  // Higher A, and a fee already expressed over a 1e10 denominator (0.2%).
  EXPECT_EQ(saber(D("5000000000000"), D("5000000000000"), 400, 20000000, 10000000000)
                .amountOut(0, 1, D("1000000000"))
                .toDec(),
            "997999502");
}

TEST(SaberStableSwapTest, TwoCoinComposition)
{
  StableSwapCurve pool = saber(D("5000000000000"), D("5000000000000"), 100, 4, 10000);
  EXPECT_EQ(pool.tokenCount(), 2u);
  const u256 out = pool.amountOut(0, 1, D("1000000000"));
  auto clone = pool.clone();
  EXPECT_EQ(pool.applySwap(0, 1, D("1000000000")).toDec(), out.toDec());
  EXPECT_EQ(pool.balances()[0].toDec(), "5001000000000");  // +dx
  EXPECT_EQ(clone->balances()[0].toDec(), "5000000000000");
}

}  // namespace
