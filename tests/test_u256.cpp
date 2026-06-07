/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/util/int/u256.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

// Vectors computed independently with Python big-int.
const std::string A = "123456789012345678901234567890123456789";
const std::string B = "98765432109876543210987654321";

TEST(U256Test, AddSub)
{
  EXPECT_EQ((u256::fromDec(A) + u256::fromDec(B)).toDec(),
            "123456789111111111011111111101111111110");
  EXPECT_EQ((u256::fromDec(A) - u256::fromDec(B)).toDec(),
            "123456788913580246791358024679135802468");
}

TEST(U256Test, MulFits256)
{
  EXPECT_EQ((u256::fromDec(A) * u256::fromDec(B)).toDec(),
            "12193263113702179522618503273374485596336229233322374638011112635269");
}

TEST(U256Test, DivMod)
{
  EXPECT_EQ((u256::fromDec(A) / u256::fromDec(B)).toDec(), "1249999988");
  EXPECT_EQ((u256::fromDec(A) % u256::fromDec(B)).toDec(), "60185185206018518520725308641");
}

// The product 10^120 exceeds 256 bits; mulDiv carries it in 512.
TEST(U256Test, MulDivWideProduct)
{
  const u256 x = u256::pow10(60);
  const u256 y = u256::pow10(60);
  const u256 c = u256::pow10(70);
  EXPECT_EQ(mulDiv(x, y, c).toDec(), u256::pow10(50).toDec());
  EXPECT_EQ(mulDivUp(x, y, c).toDec(), u256::pow10(50).toDec());  // exact, no round up
}

TEST(U256Test, MulDivRoundsCorrectly)
{
  // 7 * 1 / 2 = 3.5 -> floor 3, ceil 4
  EXPECT_EQ(mulDiv(u256(7), u256(1), u256(2)).toDec(), "3");
  EXPECT_EQ(mulDivUp(u256(7), u256(1), u256(2)).toDec(), "4");
}

// 2^200 * 2^200 / 2^160 = 2^240: the product (2^400) overflows 256 and lives in
// the 512-bit intermediate, exercising the restoring division across both words.
TEST(U256Test, PowerOfTwoMulDiv)
{
  const u256 p200 =
      u256::fromDec("1606938044258990275541962092341162602522202993782792835301376");
  const u256 p160 = u256::fromDec("1461501637330902918203684832716283019655932542976");
  const u256 p240 = u256::fromDec(
      "1766847064778384329583297500742918515827483896875618958121606201292619776");
  EXPECT_EQ(mulDiv(p200, p200, p160).toDec(), p240.toDec());
}

TEST(U256Test, MaxValueAndHex)
{
  const u256 mx = u256::fromDec(
      "115792089237316195423570985008687907853269984665640564039457584007913129639935");
  EXPECT_EQ(mx, u256(0) - u256(1));  // 2^256 - 1, wraps
  const std::string h = "0xdeadbeef00000000000000000000000000000000000000000000000000000001";
  EXPECT_EQ(u256::fromHex(h).toDec(),
            "100720434702924942364018397558880508427273416251376888068364465368051161759745");
  EXPECT_EQ(u256::fromHex(h).toHex(), h);
}

TEST(U256Test, EdgeCases)
{
  const u256 v = u256::fromDec(A);
  EXPECT_EQ((v / u256(1)).toDec(), A);  // div by 1
  EXPECT_EQ((v / v).toDec(), "1");      // div by self
  EXPECT_TRUE((v % v).isZero());
  EXPECT_TRUE(u256(0).isZero());
  EXPECT_LT(u256::fromDec(B), u256::fromDec(A));
}

// The wei<->human boundary needs the decimals; the same raw value means
// different things at 6 vs 18 decimals.
TEST(U256Test, DecimalStringWithDecimals)
{
  EXPECT_EQ(u256::fromDec("1500000000000000000").toDecimalString(18), "1.5");
  EXPECT_EQ(u256::fromDec("1000000").toDecimalString(6), "1");
  EXPECT_EQ(u256::fromDec("123456").toDecimalString(6), "0.123456");
  EXPECT_EQ(u256::fromDec("250000").toDecimalString(6), "0.25");
}

TEST(U256Test, Literal)
{
  EXPECT_EQ(1000000000000000000_u256, u256::pow10(18));
}

}  // namespace
