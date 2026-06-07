/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/util/int/i256.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

std::string dec(const i256& v) { return (v.neg && !v.isZero() ? "-" : "") + v.magnitude().toDec(); }

TEST(I256Test, SignedAddSub)
{
  EXPECT_EQ(dec(i256(5) + i256(-3)), "2");
  EXPECT_EQ(dec(i256(-5) + i256(3)), "-2");
  EXPECT_EQ(dec(i256(-5) + i256(-3)), "-8");
  EXPECT_EQ(dec(i256(3) - i256(5)), "-2");
  EXPECT_EQ(dec(i256(-3) - i256(-5)), "2");
}

TEST(I256Test, SignedMul)
{
  EXPECT_EQ(dec(i256(-4) * i256(3)), "-12");
  EXPECT_EQ(dec(i256(-4) * i256(-3)), "12");
  EXPECT_EQ(dec(i256(0) * i256(-3)), "0");
}

// Solidity int256 division truncates toward zero, not floor.
TEST(I256Test, TruncatingDivision)
{
  EXPECT_EQ(dec(i256(-7) / i256(2)), "-3");  // floor would be -4
  EXPECT_EQ(dec(i256(7) / i256(-2)), "-3");
  EXPECT_EQ(dec(i256(-7) / i256(-2)), "3");
  EXPECT_EQ(dec(i256(7) / i256(2)), "3");
}

TEST(I256Test, TruncatingMod)
{
  EXPECT_EQ(dec(i256(-7) % i256(2)), "-1");  // -7 - 2*(-3) = -1
  EXPECT_EQ(dec(i256(7) % i256(-2)), "1");
}

TEST(I256Test, Comparisons)
{
  EXPECT_TRUE(i256(-5) < i256(3));
  EXPECT_TRUE(i256(-5) < i256(-3));
  EXPECT_TRUE(i256(3) > i256(-5));
  EXPECT_TRUE(i256(-3) == i256(-3));
  EXPECT_TRUE(i256(0) == i256(0));
}

TEST(I256Test, FromDecAndMagnitude)
{
  const i256 v = i256::fromDec("-123456789012345678901234567890");
  EXPECT_TRUE(v.neg);
  EXPECT_EQ(v.magnitude().toDec(), "123456789012345678901234567890");
}

}  // namespace
