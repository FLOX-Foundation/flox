/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The portable multiply-and-divide, exercised on a platform that does not need
 * it.
 *
 * Every fixed-point operator that turns a price and a quantity into money goes
 * through mulDivI64. On a compiler with a 128-bit integer it uses one; without
 * one it uses the software path. That software path used to live behind #else,
 * so it was never compiled anywhere this project builds -- and the hand-rolled
 * fallback it sat next to in common.h was wrong: its remainder term overflows
 * int64 at a price of 60000 and a quantity of 0.5. clang-cl landed on that
 * branch and a backtest came back with -534 where 5000 was expected.
 *
 * So the software path is compiled everywhere now, reachable by name, and
 * checked here against the hardware one on the values that matter: large
 * products, negatives on each side, and the boundaries.
 */
#include "flox/common.h"
#include "flox/util/base/scale_check.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace flox;

namespace
{
constexpr int64_t kMax = (std::numeric_limits<int64_t>::max)();
constexpr int64_t kMin = (std::numeric_limits<int64_t>::min)();
}  // namespace

// The case that was actually wrong in production code: 60000 at 0.5, whose
// intermediate product is 3e20 and does not fit an int64.
TEST(FixedPointMulDiv, TheProductThatDoesNotFitAnInt64IsExact)
{
  const int64_t price = 60000LL * 100'000'000LL;  // 6e12 raw
  const int64_t qty = 50'000'000LL;               // 0.5 raw
  const int64_t scale = 100'000'000LL;

  EXPECT_EQ(mulDivI64(qty, price, scale), 30000LL * 100'000'000LL);
  EXPECT_EQ(mulDivI64Portable(qty, price, scale), mulDivI64(qty, price, scale));

  // And through the operator a caller actually writes.
  const Volume v = Quantity::fromRaw(qty) * Price::fromRaw(price);
  EXPECT_EQ(v.raw(), 30000LL * 100'000'000LL);
}

// Signs, on every side. The MSVC intrinsic path used to cast a signed value
// straight to uint64_t, so a negative volume became 1.8e19 -- which is the
// kind of wrong that shows up as a PnL nobody can explain.
TEST(FixedPointMulDiv, EverySignCombinationAgreesWithTheHardwarePath)
{
  const int64_t values[] = {1, -1, 7, -7, 100'000'000, -100'000'000,
                            6'000'000'000'000LL, -6'000'000'000'000LL};
  for (int64_t a : values)
  {
    for (int64_t b : values)
    {
      for (int64_t d : values)
      {
        EXPECT_EQ(mulDivI64Portable(a, b, d), mulDivI64(a, b, d))
            << "a=" << a << " b=" << b << " d=" << d;
      }
    }
  }
}

TEST(FixedPointMulDiv, RandomValuesAgreeWithTheHardwarePath)
{
  std::mt19937_64 rng(20260919);
  std::uniform_int_distribution<int64_t> wide(kMin / 4, kMax / 4);
  std::uniform_int_distribution<int64_t> small(-1'000'000'000LL, 1'000'000'000LL);

  for (int i = 0; i < 20000; ++i)
  {
    const int64_t a = (i % 2) ? wide(rng) : small(rng);
    const int64_t b = small(rng);
    int64_t d = small(rng);
    if (d == 0)
    {
      d = 1;
    }
    EXPECT_EQ(mulDivI64Portable(a, b, d), mulDivI64(a, b, d))
        << "a=" << a << " b=" << b << " d=" << d;
  }
}

// Saturation rather than wrapping, and the same answer from both paths. A
// wrapped quotient is a number that looks plausible; a saturated one is
// obviously at the edge.
TEST(FixedPointMulDiv, AQuotientPastTheEdgeSaturatesTheSameWayInBothPaths)
{
  EXPECT_EQ(mulDivI64Portable(kMax, kMax, 1), mulDivI64(kMax, kMax, 1));
  EXPECT_EQ(mulDivI64Portable(kMax, 2, 1), mulDivI64(kMax, 2, 1));
  EXPECT_EQ(mulDivI64Portable(kMin, 2, 1), mulDivI64(kMin, 2, 1));
  EXPECT_EQ(mulDivI64Portable(kMin, -1, 1), mulDivI64(kMin, -1, 1));
}

// A zero divisor has no representable answer, and the two paths must agree on
// what they do about it rather than each improvising.
TEST(FixedPointMulDiv, AZeroDivisorIsHandledIdenticallyByBothPaths)
{
  EXPECT_EQ(mulDivI64Portable(0, 0, 0), mulDivI64(0, 0, 0));
  EXPECT_EQ(mulDivI64Portable(5, 7, 0), mulDivI64(5, 7, 0));
  EXPECT_EQ(mulDivI64Portable(-5, 7, 0), mulDivI64(-5, 7, 0));
}
