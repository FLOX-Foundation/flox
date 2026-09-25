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

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace flox;

namespace
{
constexpr int64_t kMax = (std::numeric_limits<int64_t>::max)();
constexpr int64_t kMin = (std::numeric_limits<int64_t>::min)();

// A value the optimizer cannot see through, so a clamp is decided at runtime
// by the code under test rather than folded away by the compiler. Same reason
// as the identical helper in tests/test_decimal_portable.cpp.
double opaque(double v)
{
  volatile double sink = v;
  return sink;
}
}  // namespace

// ---------------------------------------------------------------------------
// The narrowing clamps, at the exact values they compare against.
// ---------------------------------------------------------------------------
//
// Both clamps in scale_check.h are written against a boundary the tests above
// only ever approach: narrowDoubleToI64 against 2^63 (the power of two, since
// INT64_MAX itself is not a double), and checkedNarrowI64 against INT64_MIN
// and INT64_MAX. A comparison that is one notch too loose or too tight is
// invisible anywhere but on the boundary value itself, so each is decided
// here at the threshold and at the representable value next to it.

TEST(FixedPointNarrowing, TheDoubleClampIsExactAtBothBounds)
{
  constexpr double kUpper = 9223372036854775808.0;   // 2^63, exactly
  constexpr double kLower = -9223372036854775808.0;  // -2^63, exactly

  // 2^63 is one past INT64_MAX and must clamp; the double just below it is
  // INT64_MAX - 1023 and must survive the cast untouched.
  EXPECT_EQ(narrowDoubleToI64(opaque(kUpper)), kMax);
  EXPECT_EQ(narrowDoubleToI64(opaque(std::nextafter(kUpper, 0.0))), 9223372036854774784LL);

  // -2^63 IS INT64_MIN, so the lower bound is inclusive: it must not clamp
  // away, and the double just above it must survive as well.
  EXPECT_EQ(narrowDoubleToI64(opaque(kLower)), kMin);
  EXPECT_EQ(narrowDoubleToI64(opaque(std::nextafter(kLower, 0.0))), -9223372036854774784LL);

  // And a step past each bound, which is the side the clamp exists for.
  EXPECT_EQ(narrowDoubleToI64(opaque(std::nextafter(kUpper, 1e300))), kMax);
  EXPECT_EQ(narrowDoubleToI64(opaque(std::nextafter(kLower, -1e300))), kMin);
}

#if defined(__SIZEOF_INT128__)
TEST(FixedPointNarrowing, TheI128ClampIsExactAtBothBounds)
{
  // On the bounds themselves nothing is out of range: neither the check nor
  // the saturation may fire, and the value has to come back unchanged.
  EXPECT_EQ(checkedNarrowI64(static_cast<__int128_t>(kMax)), kMax);
  EXPECT_EQ(checkedNarrowI64(static_cast<__int128_t>(kMin)), kMin);
  EXPECT_EQ(checkedNarrowI64(static_cast<__int128_t>(kMax) - 1), kMax - 1);
  EXPECT_EQ(checkedNarrowI64(static_cast<__int128_t>(kMin) + 1), kMin + 1);
}
#endif

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
  // Values whose products fit: what is under test here is the SIGN, and
  // overflow is covered by build above. Leaving 6e12 in made a checked build
  // abort on the pair whose quotient exceeds int64 -- correctly, and not on
  // the question this test is asking.
  const int64_t values[] = {1, -1, 7, -7, 100'000'000, -100'000'000};
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
  // Deliberately inside the range whose product fits: overflow is covered
  // above, by build, and mixing it in here would abort a checked build on a
  // random draw rather than on purpose.
  std::uniform_int_distribution<int64_t> wide(-4'000'000'000LL, 4'000'000'000LL);
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

// Overflow behaves differently by build, and both halves are the contract.
//
// With scale checks on (debug, and CI's sanitizer jobs) the assert fires
// before saturation gets a chance: a quotient past int64 is a programming
// error and the build says so. With them off it saturates -- defined, at the
// edge, and obviously at the edge rather than wrapped into a plausible
// number. The first version of this test asserted only the second half, and
// aborted every checked build it met.
#if FLOX_SCALE_CHECKS
TEST(FixedPointMulDivDeathTest, AQuotientPastTheEdgeTrapsWhenChecksAreOn)
{
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  // The two paths word it differently, and truthfully so: the hardware one
  // forms a 128-bit intermediate and trips while narrowing it, the portable
  // one never forms one and trips on the quotient. Matching the word they
  // share keeps the test about the trap rather than about the phrasing.
  EXPECT_DEATH({ (void)mulDivI64(kMax, kMax, 1); }, "overflow");
  EXPECT_DEATH({ (void)mulDivI64Portable(kMax, kMax, 1); }, "overflow");
}
#else
TEST(FixedPointMulDiv, AQuotientPastTheEdgeSaturatesTheSameWayInBothPaths)
{
  EXPECT_EQ(mulDivI64Portable(kMax, kMax, 1), mulDivI64(kMax, kMax, 1));
  EXPECT_EQ(mulDivI64Portable(kMax, 2, 1), mulDivI64(kMax, 2, 1));
  EXPECT_EQ(mulDivI64Portable(kMin, 2, 1), mulDivI64(kMin, 2, 1));
  EXPECT_EQ(mulDivI64Portable(kMin, -1, 1), mulDivI64(kMin, -1, 1));
  EXPECT_EQ(mulDivI64(kMax, kMax, 1), kMax) << "it wrapped instead of stopping at the edge";
}
#endif

// The quotient bounds, at the exact values the guards compare against.
//
// A quotient of INT64_MAX is not an overflow, and on the negative side
// neither is 2^63 -- INT64_MIN has no positive twin, so the negative branch
// allows one more than the positive one. Both guards are written `>`, and a
// `>=` in either place turns the largest legal answer into a reported
// overflow: same number in a release build, an abort in a checked one. These
// run in both builds and must not die in either.
TEST(FixedPointMulDiv, TheLargestQuotientsThatAreNotOverflowsAreAccepted)
{
  EXPECT_EQ(mulDivI64Portable(kMax, 1, 1), kMax);
  EXPECT_EQ(mulDivI64Portable(kMin, 1, 1), kMin);
  EXPECT_EQ(mulDivI64Portable(1, kMax, 1), kMax);
  EXPECT_EQ(mulDivI64Portable(-1, kMax, 1), kMin + 1);
  EXPECT_EQ(mulDivI64Portable(kMax, 1, 1), mulDivI64(kMax, 1, 1));
  EXPECT_EQ(mulDivI64Portable(kMin, 1, 1), mulDivI64(kMin, 1, 1));
}

// A zero divisor has no representable answer, and the two builds answer
// differently on purpose: checked says so and stops, release hands back the
// saturated value both paths agree on. Either way the two paths must not
// improvise separately.
#if FLOX_SCALE_CHECKS
TEST(FixedPointMulDivDeathTest, AZeroDivisorTrapsInBothPathsWhenChecksAreOn)
{
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH({ (void)mulDivI64(5, 7, 0); }, "division by zero");
  EXPECT_DEATH({ (void)mulDivI64Portable(5, 7, 0); }, "division by zero");
}
#else
TEST(FixedPointMulDiv, AZeroDivisorIsHandledIdenticallyByBothPaths)
{
  EXPECT_EQ(mulDivI64Portable(0, 0, 0), mulDivI64(0, 0, 0));
  EXPECT_EQ(mulDivI64Portable(5, 7, 0), mulDivI64(5, 7, 0));
  EXPECT_EQ(mulDivI64Portable(-5, 7, 0), mulDivI64(-5, 7, 0));
}

// The two paths agreeing says nothing about WHAT they agree on: both read the
// sign of the product they never formed, and a saturated value pointing the
// wrong way (or a zero where the boundary belongs) is exactly as consistent.
// The saturated answer carries the product's sign, and a zero operand is not
// a sign at all -- it is a zero, and has no boundary to point at.
TEST(FixedPointMulDiv, AZeroDivisorSaturatesWithTheProductsOwnSign)
{
  EXPECT_EQ(mulDivI64(5, 7, 0), kMax);
  EXPECT_EQ(mulDivI64Portable(5, 7, 0), kMax);
  EXPECT_EQ(mulDivI64(-5, -7, 0), kMax);
  EXPECT_EQ(mulDivI64Portable(-5, -7, 0), kMax);

  EXPECT_EQ(mulDivI64(-5, 7, 0), kMin);
  EXPECT_EQ(mulDivI64Portable(-5, 7, 0), kMin);
  EXPECT_EQ(mulDivI64(5, -7, 0), kMin);
  EXPECT_EQ(mulDivI64Portable(5, -7, 0), kMin);

  EXPECT_EQ(mulDivI64(0, 7, 0), 0);
  EXPECT_EQ(mulDivI64Portable(0, 7, 0), 0);
  EXPECT_EQ(mulDivI64(5, 0, 0), 0);
  EXPECT_EQ(mulDivI64Portable(5, 0, 0), 0);
  EXPECT_EQ(mulDivI64(0, 0, 0), 0);
  EXPECT_EQ(mulDivI64Portable(0, 0, 0), 0);
}

// The software path's overflow guard, at the exact high word it trips on.
//
// The shift-subtract below the guard only ever records quotient bits under
// 64, so it silently answers 0 for a quotient that needs bit 64 -- the guard
// is what keeps that from being returned as a price. It reads `hi >= ud`, and
// hi == ud is precisely a quotient of 2^64: loosen it to `hi > ud` and a
// saturated INT64_MAX becomes a plausible-looking zero. Nothing above reaches
// this threshold; the products there are either far under it or far past it.
TEST(FixedPointMulDiv, TheHighWordGuardTripsWhenItReachesTheDivisor)
{
  // 6 * 2^32 * 2^32 = 6 * 2^64, divided by 6: hi == ud exactly.
  EXPECT_EQ(mulDivI64Portable(6LL << 32, 1LL << 32, 6), kMax);
  EXPECT_EQ(mulDivI64Portable(-(6LL << 32), 1LL << 32, 6), kMin);
  EXPECT_EQ(mulDivI64Portable(6LL << 32, 1LL << 32, 6), mulDivI64(6LL << 32, 1LL << 32, 6));

  // And under the guard, where the shift-subtract is the one that answers:
  // 60000.12345678 * 60000 carries a high word of ~1.95e6 against a divisor
  // of 1e8, so it goes through the divide rather than the saturation.
  EXPECT_EQ(mulDivI64Portable(6'000'012'345'678LL, 6'000'000'000'000LL, 100'000'000LL),
            360'000'740'740'680'000LL);
}
#endif
