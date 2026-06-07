/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/constant_product_curve.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

// Vectors computed independently with Python integer math, matching Uniswap V2
// getAmountsOut / PancakeSwap to the wei.
TEST(ConstantProductCurveTest, UniswapV2ToTheWei)
{
  // USDC(6) reserve 8,000,000 ; WETH(18) reserve 5,000 ; fee 997/1000.
  ConstantProductCurve pool(u256::fromDec("8000000000000"), u256::fromDec("5000000000000000000000"),
                            997, 1000);
  EXPECT_EQ(pool.amountOut(0, 1, u256::fromDec("1000000000")).toDec(), "623047352723666813");
  EXPECT_EQ(pool.amountOut(0, 1, u256::fromDec("100000000000")).toDec(), "61545489339111325110");
  // Reverse: 1 WETH -> USDC.
  EXPECT_EQ(pool.amountOut(1, 0, u256::fromDec("1000000000000000000")).toDec(), "1594881980");
}

TEST(ConstantProductCurveTest, PancakeFeeToTheWei)
{
  // WBNB(18) reserve 10,000 ; USDT(18) reserve 6,000,000 ; fee 9975/10000.
  ConstantProductCurve pool(u256::fromDec("10000000000000000000000"),
                            u256::fromDec("6000000000000000000000000"), 9975, 10000);
  EXPECT_EQ(pool.amountOut(0, 1, u256::fromDec("10000000000000000000")).toDec(),
            "5979035911678101094158");
}

TEST(ConstantProductCurveTest, ApplySwapMovesReserves)
{
  ConstantProductCurve pool(u256::fromDec("1000000"), u256::fromDec("1000000"), 997, 1000);
  const u256 in = u256::fromDec("10000");
  const u256 out = pool.amountOut(0, 1, in);
  EXPECT_EQ(pool.applySwap(0, 1, in).toDec(), out.toDec());
  EXPECT_EQ(pool.balances()[0].toDec(), "1010000");
  EXPECT_EQ((pool.balances()[1] + out).toDec(), "1000000");
}

TEST(ConstantProductCurveTest, ZeroInputZeroOut)
{
  ConstantProductCurve pool(u256::fromDec("1000000"), u256::fromDec("1000000"), 997, 1000);
  EXPECT_TRUE(pool.amountOut(0, 1, u256(0)).isZero());
}

TEST(ConstantProductCurveTest, CloneIsIndependent)
{
  ConstantProductCurve pool(u256::fromDec("1000000"), u256::fromDec("1000000"), 997, 1000);
  auto copy = pool.clone();
  copy->applySwap(0, 1, u256::fromDec("100000"));
  EXPECT_EQ(pool.balances()[0].toDec(), "1000000");   // original intact
  EXPECT_EQ(copy->balances()[0].toDec(), "1100000");  // clone moved
  EXPECT_EQ(pool.tokenCount(), 2u);
}

}  // namespace
