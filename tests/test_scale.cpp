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

}  // namespace
