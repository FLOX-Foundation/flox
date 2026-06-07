/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/raydium_clmm_curve.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

u256 D(const char* s) { return u256::fromDec(s); }

// Expected values come from a faithful Python transcription of the Raydium CLMM
// compute_swap + tick walk (the program's nested get_delta_amount_0 rounding); the
// C++ reproduces them exactly. The curve is additionally checked against a live
// Raydium CLMM pool via Jupiter in the dex-lab harness. sqrt price 1.0 is 2^64.
const char* SP = "18446744073709551616";  // 2^64
const char* L = "5000000000000";          // active liquidity
const uint32_t FEE = 3000;                // 0.30%

TEST(RaydiumClmmCurveTest, NoCross)
{
  RaydiumClmmCurve up(D(SP), D(L), FEE, {});
  RaydiumClmmCurve down(D(SP), D(L), FEE, {});
  EXPECT_EQ(up.tokenCount(), 2u);
  EXPECT_EQ(up.amountOut(1, 0, D("1000000000")).toDec(), "996801237");
  EXPECT_EQ(down.amountOut(0, 1, D("1000000000")).toDec(), "996801237");
}

TEST(RaydiumClmmCurveTest, CrossDownward)
{
  std::vector<ClTick> ticks{{D("18428297329635842064"), i256(D("2000000000000"), false)}};
  RaydiumClmmCurve pool(D(SP), D(L), FEE, ticks);
  EXPECT_EQ(pool.amountOut(0, 1, D("50000000000")).toDec(), "49096834908");
}

TEST(RaydiumClmmCurveTest, CrossUpward)
{
  std::vector<ClTick> ticks{{D("18465190817783261167"), i256(D("1500000000000"), false)}};
  RaydiumClmmCurve pool(D(SP), D(L), FEE, ticks);
  EXPECT_EQ(pool.amountOut(1, 0, D("50000000000")).toDec(), "49449013134");
}

TEST(RaydiumClmmCurveTest, ApplyAndClone)
{
  RaydiumClmmCurve pool(D(SP), D(L), FEE, {});
  const u256 out = pool.amountOut(0, 1, D("1000000000"));
  auto clone = pool.clone();
  EXPECT_EQ(pool.applySwap(0, 1, D("1000000000")).toDec(), out.toDec());
  EXPECT_NE(pool.sqrtPrice().toDec(), D(SP).toDec());  // price moved down
  EXPECT_EQ(clone->amountOut(0, 1, D("1000000000")).toDec(), out.toDec());
}

}  // namespace
