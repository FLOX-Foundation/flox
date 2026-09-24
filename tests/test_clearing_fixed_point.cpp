/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Two things about Account and LeveragedPosition.
//
// First: closePosition() erases the legs for a symbol and never touches
// equity, so a position that was marked into profit or loss realises nothing.
// The account's equity only ever moves through an explicit addEquity, which
// means a backtest or a venue ledger that opens and closes positions reports
// the same equity it started with.
//
// Second: quantity, entry price, equity, marks and the 30-day rolling
// notional are all double, accumulated with += and -=, and the rolling total
// is clamped to zero when the drift takes it negative -- which hides the
// drift rather than removing it. The tests below use sequences whose exact
// answer is representable in the 1e-8 fixed point and where the double
// accumulation is visibly not that answer.
//
// The helpers below reach the Account through whichever signature it has, so
// the tests keep their meaning whether the fix keeps the double-facing API or
// moves it to Quantity / Price / Volume.

#include "flox/clearing/account.h"
#include "flox/clearing/leveraged_position.h"
#include "flox/common.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

using namespace flox;

namespace
{

constexpr SymbolId BTC = 1;

template <class T>
int64_t moneyRaw(const T& v)
{
  if constexpr (std::is_floating_point_v<T>)
  {
    return Volume::fromDouble(v).raw();
  }
  else
  {
    return v.raw();
  }
}

template <class A = Account>
A makeAccount(uint64_t id, int64_t equityRaw)
{
  if constexpr (requires { A(id, 0.0); })
  {
    return A(id, Volume::fromRaw(equityRaw).toDouble());
  }
  else
  {
    return A(id, Volume::fromRaw(equityRaw));
  }
}

template <class A>
void openRaw(A& a, SymbolId symbol, int64_t qtyRaw, int64_t priceRaw)
{
  if constexpr (requires { a.openPosition(symbol, 0.0, 0.0); })
  {
    a.openPosition(symbol, Quantity::fromRaw(qtyRaw).toDouble(),
                   Price::fromRaw(priceRaw).toDouble());
  }
  else
  {
    a.openPosition(symbol, Quantity::fromRaw(qtyRaw), Price::fromRaw(priceRaw));
  }
}

template <class A>
void markRaw(A& a, SymbolId symbol, int64_t priceRaw)
{
  if constexpr (requires { a.setMark(symbol, 0.0); })
  {
    a.setMark(symbol, Price::fromRaw(priceRaw).toDouble());
  }
  else
  {
    a.setMark(symbol, Price::fromRaw(priceRaw));
  }
}

template <class A>
void recordRaw(A& a, int64_t tsNs, int64_t notionalRaw)
{
  if constexpr (requires { a.recordFill(tsNs, 0.0); })
  {
    a.recordFill(tsNs, Volume::fromRaw(notionalRaw).toDouble());
  }
  else
  {
    a.recordFill(tsNs, Volume::fromRaw(notionalRaw));
  }
}

constexpr int64_t kEquityRaw = 100'000'000'000'000;  // 1'000'000.0
constexpr int64_t kEntryRaw = 3'000'000'000'000;     // 30'000.0
constexpr int64_t kMarkRaw = 3'123'456'780'000;      // 31'234.5678
constexpr int64_t kQtyRaw = 250'000'000;             // 2.5

// 100'000.0 and 200'000.0000123: the difference is 100'000.0000123, and one
// tenth of it is exactly 10'000.00000123 -- no rounding anywhere in the
// fixed-point arithmetic, and two thousand of them stay well inside int64.
constexpr int64_t kLegEntryRaw = 10'000'000'000'000;
constexpr int64_t kLegMarkRaw = 20'000'000'001'230;
constexpr int64_t kLegQtyRaw = 10'000'000;  // 0.1
constexpr int64_t kLegPnlRaw = 1'000'000'000'123;
constexpr int kLegs = 2000;

}  // namespace

// Open, mark into profit, close. The profit is the account's now, and equity
// has to say so.
TEST(ClearingFixedPoint, ClosePositionRealisesTheMarkedPnlIntoEquity)
{
  auto a = makeAccount(1, kEquityRaw);
  openRaw(a, BTC, kQtyRaw, kEntryRaw);
  markRaw(a, BTC, kMarkRaw);

  a.closePosition(BTC);

  // (31'234.5678 - 30'000.0) * 2.5 = 3'086.4195
  constexpr int64_t realised = 308'641'950'000;
  EXPECT_EQ(a.positionCount(), 0u);
  EXPECT_EQ(moneyRaw(a.equity()), kEquityRaw + realised);
}

// The losing direction, so a fix cannot pass by adding an absolute value.
TEST(ClearingFixedPoint, ClosePositionRealisesALossIntoEquity)
{
  auto a = makeAccount(1, kEquityRaw);
  openRaw(a, BTC, -kQtyRaw, kEntryRaw);  // short
  markRaw(a, BTC, kMarkRaw);             // mark moved up: the short is down

  a.closePosition(BTC);

  constexpr int64_t realised = -308'641'950'000;
  EXPECT_EQ(a.positionCount(), 0u);
  EXPECT_EQ(moneyRaw(a.equity()), kEquityRaw + realised);
}

// Two thousand round trips, each realising the same exact amount. The total
// is two thousand times it, with nothing to round; a double equity that is
// incremented two thousand times is not.
TEST(ClearingFixedPoint, ClosedLegsAccumulateIntoEquityExactly)
{
  auto a = makeAccount(1, 0);

  for (int i = 0; i < kLegs; ++i)
  {
    openRaw(a, BTC, kLegQtyRaw, kLegEntryRaw);
    markRaw(a, BTC, kLegMarkRaw);
    a.closePosition(BTC);
  }

  EXPECT_EQ(a.positionCount(), 0u);
  EXPECT_EQ(moneyRaw(a.equity()), kLegPnlRaw * kLegs);
}

// The 30-day rolling notional. One large fill swallows every small one that
// follows it -- a double at 5e10 cannot hold 1e-8 -- and when the large fill
// expires the subtraction takes the total below zero, where the clamp at the
// end of evictExpired rewrites it as zero. The five hundred small fills still
// inside the window, and the one recorded last, are simply gone.
TEST(ClearingFixedPoint, RollingNotionalKeepsSmallFillsBesideALargeOne)
{
  auto a = makeAccount(1, 0);

  recordRaw(a, 0, 500'000'000'000'000'000);  // 5'000'000'000.0
  for (int64_t i = 1; i <= 1000; ++i)
  {
    recordRaw(a, i, 1);  // 0.00000001
  }
  recordRaw(a, Account::kThirtyDaysNs + 500, 7);  // 0.00000007

  // Evicted: the large fill (ts 0) and the first five hundred small ones.
  // Left in the window: five hundred small fills plus the last one.
  EXPECT_EQ(moneyRaw(a.rollingNotional30d()), 500 * 1 + 7);
}

// The same counter without the clamp in play: a thousand fills of an amount
// that a double sum cannot keep exactly.
TEST(ClearingFixedPoint, RollingNotionalSumsFillsExactly)
{
  auto a = makeAccount(1, 0);

  for (int64_t i = 0; i < kLegs; ++i)
  {
    recordRaw(a, i, kLegPnlRaw);
  }

  EXPECT_EQ(moneyRaw(a.rollingNotional30d()), kLegPnlRaw * kLegs);
}

// Aggregates over the position book. Notional and unrealised PnL are what
// the maintenance-margin check and the liquidation decision are computed
// from, so a drifting sum is a liquidation at the wrong price.
TEST(ClearingFixedPoint, TotalNotionalIsExactAcrossManyLegs)
{
  auto a = makeAccount(1, 0);
  for (int i = 0; i < kLegs; ++i)
  {
    openRaw(a, BTC, kLegQtyRaw, kLegMarkRaw);
  }

  // 200'000.0000123 * 0.1 = 20'000.00000123 per leg
  constexpr int64_t perLeg = 2'000'000'000'123;
  EXPECT_EQ(moneyRaw(a.totalNotional()), perLeg * kLegs);
  EXPECT_EQ(moneyRaw(a.marginNotional()), perLeg * kLegs);
}

TEST(ClearingFixedPoint, TotalUnrealisedPnlIsExactAcrossManyLegs)
{
  auto a = makeAccount(1, 0);
  for (int i = 0; i < kLegs; ++i)
  {
    openRaw(a, BTC, kLegQtyRaw, kLegEntryRaw);
  }
  markRaw(a, BTC, kLegMarkRaw);

  EXPECT_EQ(moneyRaw(a.totalUnrealisedPnl()), kLegPnlRaw * kLegs);
  EXPECT_EQ(moneyRaw(a.marginUnrealisedPnl()), kLegPnlRaw * kLegs);
}

// Margin and liquidation are computed from these four numbers, and they are
// the last place in the engine that should be leaving fixed point. A double
// quantity and a double entry price cannot even represent the fills that
// produced them, so every aggregate above starts from a value that is already
// approximate.
//
// needs: Quantity LeveragedPosition::quantity
// needs: Price LeveragedPosition::entryPrice
// needs: Volume LeveragedPosition::equity
// needs: Volume Account::equity() const
// needs: Volume Account::rollingNotional30d() const
//
// Stated as a type check so this file still compiles against the current
// declarations and fails at run time; every other assertion here reaches the
// values through helpers that accept either form.
TEST(ClearingFixedPoint, PositionAndEquityAreFixedPointTyped)
{
  EXPECT_TRUE((std::is_same_v<Quantity, decltype(LeveragedPosition::quantity)>))
      << "LeveragedPosition::quantity is a double";
  EXPECT_TRUE((std::is_same_v<Price, decltype(LeveragedPosition::entryPrice)>))
      << "LeveragedPosition::entryPrice is a double";
  EXPECT_TRUE((std::is_same_v<Volume, decltype(LeveragedPosition::equity)>))
      << "LeveragedPosition::equity is a double";

  using EquityT = std::remove_cvref_t<decltype(std::declval<const Account&>().equity())>;
  using RollingT =
      std::remove_cvref_t<decltype(std::declval<const Account&>().rollingNotional30d())>;
  EXPECT_TRUE((std::is_same_v<Volume, EquityT>)) << "Account::equity() returns a double";
  EXPECT_TRUE((std::is_same_v<Volume, RollingT>))
      << "Account::rollingNotional30d() returns a double";
}

// Control: the same paths at a magnitude and a leg count a double still
// carries exactly. Green before the fix and after it.
TEST(ClearingFixedPoint, ModestAggregatesAreExactToday)
{
  auto a = makeAccount(1, 100'000'000'000);
  openRaw(a, BTC, 100'000'000, 10'000'000'000);  // 1.0 @ 100.0
  markRaw(a, BTC, 11'000'000'000);               // 110.0

  EXPECT_EQ(moneyRaw(a.totalNotional()), 11'000'000'000);
  EXPECT_EQ(moneyRaw(a.totalUnrealisedPnl()), 1'000'000'000);

  recordRaw(a, 1, 10'000'000'000);
  recordRaw(a, 2, 25'000'000'000);
  EXPECT_EQ(moneyRaw(a.rollingNotional30d()), 35'000'000'000);
}
