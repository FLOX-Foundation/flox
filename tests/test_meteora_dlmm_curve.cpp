/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/meteora_dlmm_curve.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{

u256 D(const char* s) { return u256::fromDec(s); }

DlmmFeeParams params()
{
  DlmmFeeParams p;
  p.baseFactor = 10000;
  p.baseFeePowerFactor = 0;
  p.variableFeeControl = 40000;
  p.filterPeriod = 30;
  p.decayPeriod = 600;
  p.reductionFactor = 5000;
  p.maxVolatilityAccumulator = 350000;
  return p;
}

std::vector<DlmmBin> bins()
{
  std::vector<DlmmBin> b;
  for (int32_t i = -3; i <= 3; ++i)
  {
    b.push_back({i, D("1000000000"), D("1000000000")});
  }
  return b;
}

MeteoraDlmmCurve pool() { return MeteoraDlmmCurve(0, 10, bins(), params(), DlmmVolatility{}, 1000000); }

// The bin price (1 + bin_step/1e4)^id, exact against the program's Liquidity Book
// power (an approximation, not the mathematical real, so the value is the chain's).
TEST(MeteoraDlmmCurveTest, BinPriceMatchesProgram)
{
  MeteoraDlmmCurve p = pool();
  EXPECT_EQ(p.binPriceAt(0).toDec(), "18446744073709551616");   // 2^64, price 1.0
  EXPECT_EQ(p.binPriceAt(1).toDec(), "18465190817783261167");   // (1.001)^1
  EXPECT_EQ(p.binPriceAt(-1).toDec(), "18428315757951600016");  // (1.001)^-1
}

// Expected outputs come from a faithful transcription of the program's quote
// (MM-liquidity exact-in, fee-on-input); the C++ reproduces them to the unit.
TEST(MeteoraDlmmCurveTest, SwapMatchesReference)
{
  MeteoraDlmmCurve a = pool();
  EXPECT_EQ(a.amountOut(0, 1, D("500000000")).toDec(), "499500000");  // swap_for_y, within a bin
  MeteoraDlmmCurve b = pool();
  EXPECT_EQ(b.amountOut(1, 0, D("500000000")).toDec(), "499500000");  // swap_for_x
  MeteoraDlmmCurve c = pool();
  EXPECT_EQ(c.amountOut(0, 1, D("2500000000")).toDec(), "2495496549");  // crosses several bins
}

TEST(MeteoraDlmmCurveTest, BalancesAndApply)
{
  MeteoraDlmmCurve p = pool();
  EXPECT_EQ(p.tokenCount(), 2u);
  EXPECT_EQ(p.balances()[0].toDec(), "7000000000");  // 7 bins x 1e9
  EXPECT_EQ(p.balances()[1].toDec(), "7000000000");

  const u256 out = p.amountOut(0, 1, D("2500000000"));
  auto clone = p.clone();
  EXPECT_EQ(p.applySwap(0, 1, D("2500000000")).toDec(), out.toDec());
  EXPECT_EQ(clone->amountOut(0, 1, D("2500000000")).toDec(), out.toDec());  // clone untouched
}

}  // namespace
