/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/util/base/decimal.h"

#include <gtest/gtest.h>
#include <math.h>
#include <cstdint>
#include <limits>

using namespace flox;

namespace
{

struct PriceTag
{
};
using Price = Decimal<PriceTag, 1000000, 10>;

TEST(DecimalTest, FromDoubleAndToDouble)
{
  Price p = Price::fromDouble(123.456789);
  EXPECT_NEAR(p.toDouble(), 123.456789, 1e-6);
}

TEST(DecimalTest, FromRawAndRawAccess)
{
  Price p = Price::fromRaw(123456789);
  EXPECT_EQ(p.raw(), 123456789);
  EXPECT_NEAR(p.toDouble(), 123.456789, 1e-6);
}

TEST(DecimalTest, ArithmeticOperations)
{
  Price a = Price::fromDouble(100.0);
  Price b = Price::fromDouble(25.0);
  Price c = a + b;
  Price d = a - b;

  EXPECT_NEAR(c.toDouble(), 125.0, 1e-6);
  EXPECT_NEAR(d.toDouble(), 75.0, 1e-6);
}

TEST(DecimalTest, ComparisonOperators)
{
  Price a = Price::fromDouble(10.0);
  Price b = Price::fromDouble(20.0);

  EXPECT_TRUE(a < b);
  EXPECT_TRUE(b > a);
  EXPECT_TRUE(a <= b);
  EXPECT_TRUE(b >= a);
  EXPECT_TRUE(a == a);
  EXPECT_FALSE(a == b);
}

TEST(DecimalTest, RoundToTick)
{
  Price p = Price::fromDouble(103.27);
  Price rounded = p.roundToTick();
  EXPECT_EQ(rounded.raw() % Price::TickSize, 0);
  EXPECT_NEAR(rounded.toDouble(),
              103.27 - fmod(103.27 * Price::Scale, Price::TickSize) / Price::Scale, 1e-6);
}

TEST(DecimalTest, IsZero)
{
  Price zero = Price::fromRaw(0);
  Price nonZero = Price::fromDouble(0.000001);

  EXPECT_TRUE(zero.isZero());
  EXPECT_FALSE(nonZero.isZero());
}

TEST(DecimalTest, FromDoubleNegativeRoundCorrectly)
{
  EXPECT_EQ(Price::fromDouble(-0.25).raw(), -250000);
  EXPECT_EQ(Price::fromDouble(-1.0).raw(), -1000000);
  EXPECT_EQ(Price::fromDouble(-0.000001).raw(), -1);
  EXPECT_EQ(Price::fromDouble(-0.0000001).raw(), 0);
}

TEST(DecimalTest, FromDoublePositiveRoundCorrectly)
{
  EXPECT_EQ(Price::fromDouble(0.25).raw(), 250000);
  EXPECT_EQ(Price::fromDouble(1.0).raw(), 1000000);
  EXPECT_EQ(Price::fromDouble(0.000001).raw(), 1);
  EXPECT_EQ(Price::fromDouble(0.0000001).raw(), 0);
}

// A double scaled past the int64 range, or one that is not a number at all,
// reaches fromDouble from every wire surface that accepts a JSON number. The
// cast has to be defined for those, not merely defined on the machine the test
// happens to run on: casting out of range is undefined behaviour, and the two
// architectures this engine builds for pick different answers.
TEST(DecimalTest, FromDoubleOutOfRangeSaturates)
{
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();

  EXPECT_EQ(Price::fromDouble(1e300).raw(), kMax);
  EXPECT_EQ(Price::fromDouble(-1e300).raw(), kMin);
  EXPECT_EQ(Price::fromDouble(std::numeric_limits<double>::infinity()).raw(), kMax);
  EXPECT_EQ(Price::fromDouble(-std::numeric_limits<double>::infinity()).raw(), kMin);

  // 1e13 at this scale is 1e19 raw: past the ceiling on a plausible typo
  // rather than on a hostile number.
  EXPECT_EQ(Price::fromDouble(1e13).raw(), kMax);

  // Not a number has no nearest representable value; zero is the only answer
  // that cannot be mistaken for a price.
  EXPECT_EQ(Price::fromDouble(std::numeric_limits<double>::quiet_NaN()).raw(), 0);

  // The scale-aware overload goes through the same narrowing.
  EXPECT_EQ(Price::fromDouble(1e300, 1000).raw(), kMax);
  EXPECT_EQ(Price::fromDouble(std::numeric_limits<double>::quiet_NaN(), 1000).raw(), 0);

  // And ordinary values are untouched.
  EXPECT_EQ(Price::fromDouble(123.456789).raw(), 123456789);
}

// ---------------------------------------------------------------------------
// operator+= must saturate instead of silently overflowing.
//
// Overflow is guarded two ways, and this file tests both:
//   - FLOX_SCALE_CHECKS on (any build without NDEBUG, or an explicit
//     -DFLOX_SCALE_CHECKS=1 build such as CI's sanitizer job): overflow
//     trips FLOX_SCALE_CHECK's assert immediately -- a programmer error
//     caught as early as possible, same as every other FLOX_SCALE_CHECK
//     in this file's arithmetic operators.
//   - FLOX_SCALE_CHECKS off (a normal NDEBUG release build): the assert
//     compiles to nothing, so checkedAddI64's saturation is what
//     actually runs and must produce a defined, non-wrapped result.
// A single un-guarded test can only exercise one of these per build, and
// the two are mutually exclusive at compile time, so the tests below are
// split accordingly instead of assuming a particular build type.
// ---------------------------------------------------------------------------

#if FLOX_SCALE_CHECKS

TEST(DecimalTest, AccumulateOverflowTripsAssertWithScaleChecksOn)
{
  // Direct boundary case, scale checks on: the assert fires before
  // saturation ever gets a chance to run -- see
  // AccumulateSaturatesInsteadOfOverflowing below for the assert-off half
  // of this same scenario.
  EXPECT_DEATH(
      {
        Price total = Price::fromRaw(std::numeric_limits<int64_t>::max() - 100);
        total += Price::fromRaw(1000);  // pushes 900 past the int64 ceiling
      },
      "fixed-point accumulation overflow");
}

TEST(DecimalTest, AccumulateRealisticBarVolumeOverflowTripsAssertWithScaleChecksOn)
{
  // Same repeated-accumulation shape as
  // AccumulateRealisticBarVolumeOverflowSaturates below, scale checks on.
  EXPECT_DEATH(
      {
        Price total = Price::fromRaw(0);
        const Price increment = Price::fromRaw(5'000'000'000'000LL);
        for (int i = 0; i < 2'000'000; ++i)
        {
          total += increment;
        }
      },
      "fixed-point accumulation overflow");
}

TEST(DecimalTest, AccumulateNegativeOverflowTripsAssertWithScaleChecksOn)
{
  EXPECT_DEATH(
      {
        Price total = Price::fromRaw(std::numeric_limits<int64_t>::min() + 10);
        total += Price::fromRaw(-100);
      },
      "fixed-point accumulation overflow");
}

#else  // !FLOX_SCALE_CHECKS

TEST(DecimalTest, AccumulateSaturatesInsteadOfOverflowing)
{
  // Direct boundary test, scale checks off (a normal release build): a raw
  // `_raw += other._raw` here is signed integer overflow -- undefined
  // behavior, previously observed in the real Bar::volume accumulator
  // (Volume::Scale = 1e8) to flip sign at -O0 and to vanish (get
  // optimized away, printing the mathematically correct value from a
  // UB-assuming compiler) at -O1, so the same accumulation reported a
  // different result depending on how the binary was built. Post-fix,
  // the accumulator must saturate at INT64_MAX rather than wrapping,
  // regardless of optimization level.
  Price total = Price::fromRaw(std::numeric_limits<int64_t>::max() - 100);
  total += Price::fromRaw(1000);  // pushes 900 past the int64 ceiling

  EXPECT_EQ(total.raw(), std::numeric_limits<int64_t>::max());
}

TEST(DecimalTest, AccumulateRealisticBarVolumeOverflowSaturates)
{
  // Reproduces the actual finding shape: repeatedly accumulating a large
  // per-trade notional (as Bar::volume += price*quantity does) past the
  // representable range. At this test file's local Price (Scale=1e6),
  // 2,000,000 iterations of a raw increment of 5e12 accumulate to 1e19,
  // comfortably past INT64_MAX (~9.223e18).
  Price total = Price::fromRaw(0);
  const Price increment = Price::fromRaw(5'000'000'000'000LL);
  for (int i = 0; i < 2'000'000; ++i)
  {
    total += increment;
  }

  EXPECT_GE(total.raw(), 0) << "accumulation must never go negative from overflow";
  EXPECT_EQ(total.raw(), std::numeric_limits<int64_t>::max());
}

TEST(DecimalTest, AccumulateNegativeSaturatesAtMin)
{
  Price total = Price::fromRaw(std::numeric_limits<int64_t>::min() + 10);
  total += Price::fromRaw(-100);

  EXPECT_EQ(total.raw(), std::numeric_limits<int64_t>::min());
}

#endif  // FLOX_SCALE_CHECKS

TEST(DecimalTest, AccumulateStaysExactBelowTheOverflowCeiling)
{
  // Sanity check: saturation must not kick in for ordinary accumulation
  // that stays well within int64 range. Never trips the assert either,
  // so this one runs unconditionally.
  Price total = Price::fromRaw(0);
  const Price increment = Price::fromDouble(1000.0);
  for (int i = 0; i < 1000; ++i)
  {
    total += increment;
  }

  EXPECT_NEAR(total.toDouble(), 1000.0 * 1000, 1e-6);
}

}  // namespace
