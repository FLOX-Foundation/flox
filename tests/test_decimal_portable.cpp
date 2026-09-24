/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Decimal's arithmetic on a toolchain without a 128-bit integer, and its
 * overflow behaviour on every toolchain.
 *
 * GCC and Clang give Decimal::operator*, operator/ and rescale() an __int128
 * intermediate. MSVC does not, and the code those builds took was written by
 * hand and never compiled anywhere: operator* overflowed its remainder term
 * at any ordinary price, operator/ overflowed `raw * Scale` at every single
 * price-by-price division. Nothing noticed because nothing on a developer's
 * machine ever built that branch.
 *
 * This file is compiled twice -- see tests/CMakeLists.txt -- once as the host
 * toolchain would build it and once with FLOX_FORCE_PORTABLE_INT128=1, which
 * compiles the native intermediate out. Both builds run the same assertions,
 * so the path MSVC takes is checked on macOS and Linux as well.
 */
#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"
#include "flox/common.h"
#include "flox/util/base/scale_check.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

using namespace flox;

namespace
{
constexpr int64_t kMax = (std::numeric_limits<int64_t>::max)();
constexpr int64_t kMin = (std::numeric_limits<int64_t>::min)();
}  // namespace

// ---------------------------------------------------------------------------
// Fixed-point multiply / divide / rescale.
// ---------------------------------------------------------------------------

// 60000.12345678 * 60000. The intermediate product is 3.6e25, which no int64
// holds; the answer does. The hand-rolled fallback split it as
// (raw/Scale)*other + (raw%Scale)*other/Scale and its second term alone is
// 12345678 * 6e12 = 7.4e19 -- past the int64 ceiling, at a BTC price anyone
// would type.
TEST(DecimalPortable, MultiplyIsExactAtAnOrdinaryPrice)
{
  const Price a = Price::fromDouble(60000.12345678);
  const Price b = Price::fromDouble(60000.0);
  ASSERT_EQ(a.raw(), 6'000'012'345'678LL);
  ASSERT_EQ(b.raw(), 6'000'000'000'000LL);

  EXPECT_EQ((a * b).raw(), 360'000'740'740'680'000LL);
}

// Every price-by-price division: `raw * Scale` is 6e12 * 1e8 = 6e20 for one
// BTC price, so the fallback overflowed before it divided anything.
TEST(DecimalPortable, DivideIsExactAtAnOrdinaryPrice)
{
  const Price a = Price::fromDouble(60000.12345678);
  const Price b = Price::fromDouble(2.0);

  EXPECT_EQ((a / b).raw(), 3'000'006'172'839LL);
}

// rescale() had the same shape: raw * toScale before any division. A DEX raw
// at a 1e15 scale is already 1e17, so the product is 1e25.
TEST(DecimalPortable, RescaleIsExactFromADexScale)
{
  const Price fine = Price::fromRaw(123'456'789'012'345'678LL);  // 123.456789012345678 at 1e15
  const Price coarse = fine.rescale(1'000'000'000'000'000LL, Price::Scale);

  EXPECT_EQ(coarse.raw(), 12'345'678'901LL);
}

// The division the fallback got wrong is also the one a quoter writes: a
// notional over a price. Checked against the type-safe cross operator, which
// has routed through mulDivI64 since the -534 backtest.
TEST(DecimalPortable, PriceByPriceAgreesWithTheCrossTypeOperator)
{
  const int64_t priceRaw = 6'000'012'345'678LL;
  const int64_t qtyRaw = 50'000'000LL;

  const Volume viaCross = Quantity::fromRaw(qtyRaw) * Price::fromRaw(priceRaw);
  const Price viaDecimal = Price::fromRaw(qtyRaw) * Price::fromRaw(priceRaw);

  EXPECT_EQ(viaDecimal.raw(), viaCross.raw());
}

// ---------------------------------------------------------------------------
// Overflow policy on the operators that had none.
// ---------------------------------------------------------------------------
//
// operator+= has saturated at the int64 boundary since checkedAddI64 landed;
// operator-=, the binary + and -, and the scalar multiply kept the plain
// expression. Signed overflow is undefined behaviour, and the observed shape
// of it here is a sign flip: an inventory walked down past INT64_MIN reads
// back as a long book.

TEST(DecimalOverflow, MinusEqualsSaturatesInsteadOfWrapping)
{
  Volume v = Volume::fromRaw(kMin + 5);
  v -= Volume::fromRaw(10);
  EXPECT_EQ(v.raw(), kMin);
}

TEST(DecimalOverflow, PlusEqualsAndBinaryPlusAgree)
{
  Volume acc = Volume::fromRaw(kMax - 3);
  acc += Volume::fromRaw(100);
  EXPECT_EQ(acc.raw(), kMax);

  EXPECT_EQ((Volume::fromRaw(kMax - 3) + Volume::fromRaw(100)).raw(), kMax);
}

TEST(DecimalOverflow, BinaryMinusSaturatesInsteadOfWrapping)
{
  EXPECT_EQ((Volume::fromRaw(kMin + 3) - Volume::fromRaw(100)).raw(), kMin);
  EXPECT_EQ((Volume::fromRaw(kMax - 3) - Volume::fromRaw(-100)).raw(), kMax);
}

TEST(DecimalOverflow, ScalarMultiplySaturatesOnBothSides)
{
  EXPECT_EQ((Quantity::fromRaw(kMax / 2 + 10) * int64_t{4}).raw(), kMax);
  EXPECT_EQ((int64_t{4} * Quantity::fromRaw(kMax / 2 + 10)).raw(), kMax);
  EXPECT_EQ((Quantity::fromRaw(kMin / 2 - 10) * int64_t{4}).raw(), kMin);
  EXPECT_EQ((int64_t{-4} * Quantity::fromRaw(kMax / 2 + 10)).raw(), kMin);
}

// INT64_MIN / -1 is the one division whose answer does not fit, and it is
// undefined behaviour rather than a wrap -- on x86 it is a hardware trap.
TEST(DecimalOverflow, ScalarDivideByMinusOneSaturatesAtTheBoundary)
{
  EXPECT_EQ((Quantity::fromRaw(kMin) / int64_t{-1}).raw(), kMax);
  EXPECT_EQ((Quantity::fromRaw(-5) / int64_t{-1}).raw(), 5);
  EXPECT_EQ((Quantity::fromRaw(kMax) / int64_t{-1}).raw(), kMin + 1);
}

TEST(DecimalOverflow, OrdinaryValuesAreUntouchedByTheChecks)
{
  EXPECT_EQ((Volume::fromDouble(3.5) + Volume::fromDouble(1.25)).raw(),
            Volume::fromDouble(4.75).raw());
  EXPECT_EQ((Volume::fromDouble(3.5) - Volume::fromDouble(1.25)).raw(),
            Volume::fromDouble(2.25).raw());
  EXPECT_EQ((Quantity::fromDouble(1.5) * int64_t{3}).raw(), Quantity::fromDouble(4.5).raw());

  Volume acc = Volume::fromDouble(10.0);
  acc -= Volume::fromDouble(2.5);
  EXPECT_EQ(acc.raw(), Volume::fromDouble(7.5).raw());
}

// ---------------------------------------------------------------------------
// The 128-bit notional accumulator in the order book.
// ---------------------------------------------------------------------------
//
// consumeAsks/consumeBids sum take * price into an __int128 and then narrow
// the quotient with a bare static_cast -- the one fixed-point narrowing in
// the tree that did not go through checkedNarrowI64. A deep consume wraps it,
// and a wrapped notional is a negative Volume handed to risk.

namespace
{
using Book = NLevelOrderBook<>;
using BookUpdatePool = pool::Pool<BookUpdateEvent, 4>;

// One level, priced so the accumulator has to carry more than an int64 can.
// 9e10 units at 100000 is 9e15 of notional, i.e. 9e23 raw.
std::pair<Quantity, Volume> consumeOneHugeLevel(bool asks)
{
  static BookUpdatePool bookPool;
  Book book{Price::fromDouble(0.1)};

  auto handle = bookPool.acquire();
  auto& u = *handle;
  u->update.type = BookUpdateType::SNAPSHOT;
  u->update.bids.clear();
  u->update.asks.clear();
  const BookLevel level{Price::fromDouble(100'000.0), Quantity::fromRaw(9'000'000'000'000'000'000LL)};
  if (asks)
  {
    u->update.asks.push_back(level);
  }
  else
  {
    u->update.bids.push_back(level);
  }
  book.applyBookUpdate(*u);

  const Quantity need = Quantity::fromRaw(9'000'000'000'000'000'000LL);
  return asks ? book.consumeAsks(need) : book.consumeBids(need);
}
}  // namespace

TEST(OrderBookNotional, ADeepAskConsumeSaturatesInsteadOfGoingNegative)
{
  const auto [filled, notional] = consumeOneHugeLevel(true);
  EXPECT_EQ(filled.raw(), 9'000'000'000'000'000'000LL);
  EXPECT_GT(notional.raw(), 0) << "the 128-bit notional wrapped into a negative Volume";
  EXPECT_EQ(notional.raw(), kMax);
}

TEST(OrderBookNotional, ADeepBidConsumeSaturatesInsteadOfGoingNegative)
{
  const auto [filled, notional] = consumeOneHugeLevel(false);
  EXPECT_EQ(filled.raw(), 9'000'000'000'000'000'000LL);
  EXPECT_GT(notional.raw(), 0) << "the 128-bit notional wrapped into a negative Volume";
  EXPECT_EQ(notional.raw(), kMax);
}

// The ordinary case still adds up, on both paths.
TEST(OrderBookNotional, AnOrdinaryConsumeIsUnchanged)
{
  static BookUpdatePool bookPool;
  Book book{Price::fromDouble(0.1)};

  auto handle = bookPool.acquire();
  auto& u = *handle;
  u->update.type = BookUpdateType::SNAPSHOT;
  u->update.bids.clear();
  u->update.asks.clear();
  u->update.asks.push_back({Price::fromDouble(100.0), Quantity::fromDouble(1.0)});
  u->update.asks.push_back({Price::fromDouble(100.5), Quantity::fromDouble(2.0)});
  book.applyBookUpdate(*u);

  const auto [filled, notional] = book.consumeAsks(Quantity::fromDouble(3.0));
  EXPECT_EQ(filled.raw(), Quantity::fromDouble(3.0).raw());
  EXPECT_EQ(notional.raw(), Volume::fromDouble(301.0).raw());
}

// ---------------------------------------------------------------------------
// The two paths, side by side, in one binary.
// ---------------------------------------------------------------------------
//
// The double build above covers the operators as a whole program would use
// them, but which of the two inline definitions the linker keeps is not
// something a test can assert. mulPath / divPath / rescalePath take the
// choice as a template argument instead, so these are two distinct
// instantiations and the comparison is exact on any platform, whatever the
// optimizer did.

TEST(DecimalPaths, MultiplyAgreesOnThePriceThatUsedToOverflow)
{
  const Price a = Price::fromDouble(60000.12345678);
  const Price b = Price::fromDouble(60000.0);

  EXPECT_EQ(a.mulPath<true>(b).raw(), a.mulPath<false>(b).raw());
  EXPECT_EQ(a.mulPath<true>(b).raw(), 360'000'740'740'680'000LL);
}

TEST(DecimalPaths, DivideAndRescaleAgree)
{
  const Price a = Price::fromDouble(60000.12345678);
  const Price b = Price::fromDouble(2.0);
  EXPECT_EQ(a.divPath<true>(b).raw(), a.divPath<false>(b).raw());

  const Price fine = Price::fromRaw(123'456'789'012'345'678LL);
  EXPECT_EQ(fine.rescalePath<true>(1'000'000'000'000'000LL, Price::Scale).raw(),
            fine.rescalePath<false>(1'000'000'000'000'000LL, Price::Scale).raw());
}

// Signs on every side, and the fixed-point rounding with them: both paths
// truncate toward zero, and a disagreement of one unit is a disagreement.
TEST(DecimalPaths, EverySignCombinationAgrees)
{
  const int64_t raws[] = {0, 1, -1, 100'000'000, -100'000'000,
                          6'000'012'345'678LL, -6'000'012'345'678LL,
                          3'333'333'333LL, -3'333'333'333LL};
  for (int64_t x : raws)
  {
    for (int64_t y : raws)
    {
      const Price a = Price::fromRaw(x);
      const Price b = Price::fromRaw(y);
      EXPECT_EQ(a.mulPath<true>(b).raw(), a.mulPath<false>(b).raw()) << "x=" << x << " y=" << y;
      if (y != 0)
      {
        EXPECT_EQ(a.divPath<true>(b).raw(), a.divPath<false>(b).raw()) << "x=" << x << " y=" << y;
      }
    }
  }
}

TEST(DecimalPaths, RandomValuesAgree)
{
  std::mt19937_64 rng(20260925);
  // Kept inside the range whose product fits: an overflow saturates, which
  // both paths do identically but which says nothing about the arithmetic.
  std::uniform_int_distribution<int64_t> raw(-4'000'000'000'000LL, 4'000'000'000'000LL);
  std::uniform_int_distribution<int64_t> scale(1, 1'000'000'000'000'000LL);

  for (int i = 0; i < 20000; ++i)
  {
    const Price a = Price::fromRaw(raw(rng));
    int64_t divisorRaw = raw(rng);
    if (divisorRaw == 0)
    {
      divisorRaw = 1;
    }
    const Price b = Price::fromRaw(divisorRaw);
    EXPECT_EQ(a.mulPath<true>(b).raw(), a.mulPath<false>(b).raw());
    EXPECT_EQ(a.divPath<true>(b).raw(), a.divPath<false>(b).raw());

    const int64_t from = scale(rng);
    EXPECT_EQ(a.rescalePath<true>(from, Price::Scale).raw(),
              a.rescalePath<false>(from, Price::Scale).raw());
  }
}
