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

// ---------------------------------------------------------------------------
// BOOK-08: operator+= must saturate instead of silently overflowing.
// ---------------------------------------------------------------------------

TEST(DecimalTest, AccumulateSaturatesInsteadOfOverflowing)
{
  // Direct boundary test: a raw `_raw += other._raw` here is signed
  // integer overflow -- undefined behavior, previously observed in the
  // real Bar::volume accumulator (Volume::Scale = 1e8) to flip sign at
  // -O0 and to vanish (get optimized away, printing the mathematically
  // correct value from a UB-assuming compiler) at -O1, so the same
  // accumulation reported a different result depending on how the binary
  // was built. Post-fix, the accumulator must saturate at INT64_MAX
  // rather than wrapping, regardless of optimization level.
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

TEST(DecimalTest, AccumulateStaysExactBelowTheOverflowCeiling)
{
  // Sanity check: saturation must not kick in for ordinary accumulation
  // that stays well within int64 range.
  Price total = Price::fromRaw(0);
  const Price increment = Price::fromDouble(1000.0);
  for (int i = 0; i < 1000; ++i)
  {
    total += increment;
  }

  EXPECT_NEAR(total.toDouble(), 1000.0 * 1000, 1e-6);
}

TEST(DecimalTest, AccumulateNegativeSaturatesAtMin)
{
  Price total = Price::fromRaw(std::numeric_limits<int64_t>::min() + 10);
  total += Price::fromRaw(-100);

  EXPECT_EQ(total.raw(), std::numeric_limits<int64_t>::min());
}

}  // namespace
