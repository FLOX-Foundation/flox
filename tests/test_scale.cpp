/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/common.h"
#include "flox/engine/symbol_registry.h"
#include "flox/util/base/scale_check.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <limits>

using namespace flox;

namespace
{

// A DEX token price (~1e-10) and a large supply (~1e12) cannot both fit the
// default 1e8 scale in int64. A per-symbol scale picked for the value range
// round-trips through the scale-aware Decimal overloads.
TEST(ScaleTest, DexFinePriceRoundTrips)
{
  const int64_t fineScale = 1'000'000'000'000'000;  // 1e15
  const double px = 0.0000001234;                   // 1.234e-7
  Price p = Price::fromDouble(px, fineScale);
  EXPECT_NEAR(p.toDouble(fineScale), px, 1e-15);
  // Same raw under the default 1e8 scale would lose the value entirely.
}

TEST(ScaleTest, DexCoarseLargeSupplyRoundTrips)
{
  const int64_t coarseScale = 100;  // 1e2
  const double qty = 987654321098.0;
  Quantity q = Quantity::fromDouble(qty, coarseScale);
  EXPECT_NEAR(q.toDouble(coarseScale), qty, 1.0);
}

// The default (parameterless) conversion is unchanged for CEX symbols.
TEST(ScaleTest, DefaultScaleUnchanged)
{
  Price p = Price::fromDouble(123.45678901);
  EXPECT_NEAR(p.toDouble(), 123.45678901, 1e-8);
  EXPECT_EQ(Price::Scale, 100'000'000);
}

// Narrowing clamps to the int64 boundary instead of wrapping silently.
TEST(ScaleTest, SaturateCastClampsInsteadOfWrapping)
{
#if defined(__SIZEOF_INT128__)
  __int128_t huge = (__int128_t)1 << 80;
  EXPECT_EQ(saturate_cast<int64_t>(huge), (std::numeric_limits<int64_t>::max)());
  __int128_t hugeNeg = -((__int128_t)1 << 80);
  EXPECT_EQ(saturate_cast<int64_t>(hugeNeg), (std::numeric_limits<int64_t>::min)());
  EXPECT_EQ(checkedNarrowI64((__int128_t)12345), 12345);
#else
  GTEST_SKIP() << "no __int128 on this target";
#endif
}

// SymbolInfo defaults reproduce the compile-time scale exactly.
TEST(ScaleTest, SymbolInfoDefaultScaleIsOneE8)
{
  SymbolInfo info;
  EXPECT_EQ(info.priceScale, Price::Scale);
  EXPECT_EQ(info.qtyScale, Quantity::Scale);
}

TEST(ScaleTest, ValidateRejectsBadScale)
{
  SymbolInfo ok;  // defaults
  EXPECT_TRUE(validateSymbolScale(ok).has_value());

  SymbolInfo zero;
  zero.priceScale = 0;
  EXPECT_FALSE(validateSymbolScale(zero).has_value());

  SymbolInfo neg;
  neg.qtyScale = -5;
  EXPECT_FALSE(validateSymbolScale(neg).has_value());

  SymbolInfo tooBig;
  tooBig.priceScale = 2'000'000'000'000'000'000;  // 2e18 > 1e18 cap
  EXPECT_FALSE(validateSymbolScale(tooBig).has_value());
}

TEST(ScaleTest, RegisterRejectsInvalidScale)
{
  SymbolRegistry reg;
  SymbolInfo bad;
  bad.exchange = "dex";
  bad.symbol = "PEPEUSDT";
  bad.priceScale = -1;
  EXPECT_EQ(reg.registerSymbol(bad), 0u);  // 0 = invalid sentinel

  SymbolInfo good;
  good.exchange = "dex";
  good.symbol = "WIFUSDT";
  good.priceScale = 1'000'000'000'000;  // 1e12
  good.qtyScale = 100;
  SymbolId id = reg.registerSymbol(good);
  EXPECT_NE(id, 0u);
}

// v4 serialization round-trips the per-symbol scale; missing scale defaults.
TEST(ScaleTest, SerializeRoundTripsScale)
{
  SymbolRegistry reg;
  SymbolInfo info;
  info.exchange = "dex";
  info.symbol = "BONKUSDT";
  info.priceScale = 1'000'000'000'000'000;  // 1e15
  info.qtyScale = 10;
  SymbolId id = reg.registerSymbol(info);
  ASSERT_NE(id, 0u);

  auto bytes = reg.serialize();
  SymbolRegistry loaded;
  ASSERT_TRUE(loaded.deserialize(bytes));

  auto got = loaded.getSymbolInfo(id);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->priceScale, 1'000'000'000'000'000);
  EXPECT_EQ(got->qtyScale, 10);
}

#if FLOX_SCALE_CHECKS
// With scale checks on (debug / CI), mixing a per-symbol-scaled value into
// arithmetic with a default-scale value traps instead of silently
// miscomputing. This is the guardrail that would have caught the position-PnL
// mis-scale found by the external consumer.
TEST(ScaleDeathTest, MismatchedScaleArithmeticTraps)
{
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  Price fine = Price::fromDouble(0.00000004, 1'000'000'000'000'000);  // scale 1e15
  Price def = Price::fromDouble(1.0);                                 // default scale
  EXPECT_DEATH(
      {
        volatile int64_t r = (fine + def).raw();
        (void)r;
      },
      "scale mismatch");
}
#endif

// rescale bridges a per-symbol-scaled value to the default scale, so a
// default-scale component (AMM pricing, quoter, position tracking) reads it
// correctly instead of off by priceScale / default.
TEST(ScaleTest, RescaleBridgesToDefaultScale)
{
  const int64_t fine = 1'000'000'000'000'000;  // 1e15
  Price p = Price::fromDouble(0.00000004, fine);
  // Without rescale the default toDouble() misreads it (0.4 here).
  ASSERT_NEAR(p.toDouble(), 0.4, 1e-9);
  Price d = p.rescale(fine, Price::Scale);
  EXPECT_NEAR(d.toDouble(), 0.00000004, 1e-12);
}

// --- fixed-point division -------------------------------------------------

// Dividing two non-zero values is the ordinary case and must stay ordinary in
// every configuration. The zero-divisor guard used to be written inverted, so
// a build with assertions on aborted right here.
TEST(ScaleTest, DivisionOfNonZeroValuesDoesNotTrap)
{
  Price hundred = Price::fromDouble(100.0);
  Price four = Price::fromDouble(4.0);
  EXPECT_NEAR((hundred / four).toDouble(), 25.0, 1e-9);

  Volume vol = Volume::fromDouble(1000.0);
  Quantity qty = Quantity::fromDouble(40.0);
  EXPECT_NEAR((vol / qty).toDouble(), 25.0, 1e-9);
}

#if FLOX_SCALE_CHECKS
// With checks on, a zero divisor trips instead of reaching the hardware.
TEST(ScaleDeathTest, DecimalDivisionByZeroTraps)
{
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  Price hundred = Price::fromDouble(100.0);
  Price zero = Price::fromRaw(0);
  EXPECT_DEATH(
      {
        volatile int64_t r = (hundred / zero).raw();
        (void)r;
      },
      "division by zero");
}

TEST(ScaleDeathTest, VolumeDividedByZeroQuantityTraps)
{
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  Volume vol = Volume::fromDouble(1000.0);
  Quantity zero = Quantity::fromRaw(0);
  EXPECT_DEATH(
      {
        volatile int64_t r = (vol / zero).raw();
        (void)r;
      },
      "division by zero");
}
#else
// With checks off the answer is still defined, and it saturates rather than
// returning whatever the platform leaves behind. A price at the int64
// boundary is unmistakable downstream; 0.0 or 25.0, which is what the
// unguarded division returned on different builds from the same input, is
// not.
TEST(ScaleTest, DecimalDivisionByZeroIsCountedAndSaturates)
{
  resetFixedPointDivisionsByZero();

  Price hundred = Price::fromDouble(100.0);
  Price zero = Price::fromRaw(0);
  EXPECT_EQ((hundred / zero).raw(), std::numeric_limits<int64_t>::max());
  EXPECT_EQ(fixedPointDivisionsByZero(), 1u);

  Price minusHundred = Price::fromDouble(-100.0);
  EXPECT_EQ((minusHundred / zero).raw(), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(fixedPointDivisionsByZero(), 2u);

  EXPECT_EQ((zero / zero).raw(), 0);
  EXPECT_EQ(fixedPointDivisionsByZero(), 3u);

  // The scalar overload divides the raw int64 directly, which is where the
  // platform difference showed up first: AArch64 returns 0 rather than
  // trapping.
  EXPECT_EQ((hundred / int64_t{0}).raw(), std::numeric_limits<int64_t>::max());
  EXPECT_EQ(fixedPointDivisionsByZero(), 4u);

  // An ordinary division leaves the counter alone.
  EXPECT_NEAR((hundred / Price::fromDouble(4.0)).toDouble(), 25.0, 1e-9);
  EXPECT_EQ(fixedPointDivisionsByZero(), 4u);

  resetFixedPointDivisionsByZero();
}

TEST(ScaleTest, VolumeDividedByZeroQuantityIsCountedAndSaturates)
{
  resetFixedPointDivisionsByZero();

  Volume vol = Volume::fromDouble(1000.0);
  Quantity zero = Quantity::fromRaw(0);
  EXPECT_EQ((vol / zero).raw(), std::numeric_limits<int64_t>::max());
  EXPECT_EQ(fixedPointDivisionsByZero(), 1u);

  Volume negative = Volume::fromDouble(-1000.0);
  EXPECT_EQ((negative / zero).raw(), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(fixedPointDivisionsByZero(), 2u);

  Price zeroPx = Price::fromRaw(0);
  EXPECT_EQ((vol / zeroPx).raw(), std::numeric_limits<int64_t>::max());
  EXPECT_EQ(fixedPointDivisionsByZero(), 3u);

  resetFixedPointDivisionsByZero();
}
#endif

}  // namespace
