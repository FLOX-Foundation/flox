/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// PositionTracker keeps the cost basis and the realised PnL in double:
// avgEntryPrice() accumulates quantity and notional as doubles, addLot()
// recomputes the average basis through Price::fromDouble on every fill, and
// closePosition() computes the per-lot PnL as
// `(closePrice.toDouble() - lot.price.toDouble()) * qty.toDouble()`.
//
// Every one of those steps is a rounding step, so the book drifts off the
// value the fixed-point types were chosen to guarantee, and the amount of
// drift depends on the compiler, on FMA contraction and on the order the
// lots happen to be in. The tests below assert exact raws over sequences
// where the drift is visible; the arithmetic they describe is exactly
// representable in the 1e-8 fixed point, so an implementation that stays in
// Decimal produces the expected raw with nothing to round.
//
// Prices here sit above 1e8 raw units of magnitude on purpose: that is where
// a double stops being able to hold eight decimals of an already large price,
// which is precisely the condition the fixed-point types exist for.

#include "flox/execution/order.h"
#include "flox/position/position_tracker.h"

#include <gtest/gtest.h>

#include <type_traits>

using namespace flox;

namespace
{

constexpr SymbolId SYM = 7;

// 61'234'567.89012345 -- eight decimals on a price whose raw no longer fits a
// double mantissa.
constexpr int64_t kOpenPriceRaw = 6'123'456'789'012'345;
// 98'765'432.10987654
constexpr int64_t kClosePriceRaw = 9'876'543'210'987'654;
// 61'876'543.21098765
constexpr int64_t kNearClosePriceRaw = 6'187'654'321'098'765;

void fill(PositionTracker& t, Side side, int64_t priceRaw, int64_t qtyRaw)
{
  Order o{};
  o.symbol = SYM;
  o.side = side;
  o.price = Price::fromRaw(priceRaw);
  o.quantity = Quantity::fromRaw(qtyRaw);
  t.onOrderFilled(o, Quantity::fromRaw(qtyRaw), Price::fromRaw(priceRaw));
}

}  // namespace

// A thousand buys of 0.1 at one price. The weighted average of a single price
// is that price, whatever the weights; the double accumulation in
// avgEntryPrice() returns something else.
TEST(PositionTrackerFixedPoint, AvgEntryPriceIsExactAfterAThousandIdenticalFills)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  for (int i = 0; i < 1000; ++i)
  {
    fill(tracker, Side::BUY, kOpenPriceRaw, 10'000'000);  // 0.1
  }

  EXPECT_EQ(tracker.getPosition(SYM).raw(), 10'000'000'000);  // 100.0
  EXPECT_EQ(tracker.getAvgEntryPrice(SYM).raw(), kOpenPriceRaw);

  auto reported = tracker.getAverageEntryPrice(SYM);
  ASSERT_TRUE(reported.has_value());
  EXPECT_EQ(reported->raw(), kOpenPriceRaw);
}

// FIFO realisation over a thousand lots. Each lot realises exactly
// (close - open) * 1.0, which is the raw difference of the two prices; the
// total is a thousand times that and has no rounding in it at all.
TEST(PositionTrackerFixedPoint, RealizedPnlIsExactAcrossAThousandLots)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  for (int i = 0; i < 1000; ++i)
  {
    fill(tracker, Side::BUY, kOpenPriceRaw, 100'000'000);  // 1.0
  }
  fill(tracker, Side::SELL, kClosePriceRaw, 100'000'000'000);  // 1000.0

  EXPECT_EQ(tracker.getPosition(SYM).raw(), 0);

  const int64_t perLot = kClosePriceRaw - kOpenPriceRaw;
  const int64_t expected = perLot * 1000;
  EXPECT_EQ(tracker.getRealizedPnl(SYM).raw(), expected);
  EXPECT_EQ(tracker.getTotalRealizedPnl().raw(), expected);
}

// AVERAGE cost basis re-derives the running average on every fill, so the
// rounding compounds instead of being confined to one step.
TEST(PositionTrackerFixedPoint, AverageCostBasisStaysExactAcrossAThousandFills)
{
  PositionTracker tracker(1, CostBasisMethod::AVERAGE);

  for (int i = 0; i < 1000; ++i)
  {
    fill(tracker, Side::BUY, kOpenPriceRaw, 10'000'000);  // 0.1
  }

  EXPECT_EQ(tracker.getPosition(SYM).raw(), 10'000'000'000);  // 100.0
  EXPECT_EQ(tracker.getAvgEntryPrice(SYM).raw(), kOpenPriceRaw);
}

// The drifted basis is then realised: the error in the cost basis is
// multiplied by the closing quantity and lands in realised PnL.
TEST(PositionTrackerFixedPoint, AverageModeRealizedPnlIsExact)
{
  PositionTracker tracker(1, CostBasisMethod::AVERAGE);

  for (int i = 0; i < 1000; ++i)
  {
    fill(tracker, Side::BUY, kOpenPriceRaw, 10'000'000);  // 0.1
  }
  fill(tracker, Side::SELL, kNearClosePriceRaw, 10'000'000'000);  // 100.0

  EXPECT_EQ(tracker.getPosition(SYM).raw(), 0);

  const int64_t expected = (kNearClosePriceRaw - kOpenPriceRaw) * 100;
  EXPECT_EQ(tracker.getRealizedPnl(SYM).raw(), expected);
}

// A short opened and covered: the sign path realises the same magnitude with
// the opposite sign, and it has to be exact too.
TEST(PositionTrackerFixedPoint, ShortRealizedPnlIsExactAcrossAThousandLots)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  for (int i = 0; i < 1000; ++i)
  {
    fill(tracker, Side::SELL, kClosePriceRaw, 100'000'000);  // 1.0
  }
  fill(tracker, Side::BUY, kOpenPriceRaw, 100'000'000'000);  // 1000.0

  EXPECT_EQ(tracker.getPosition(SYM).raw(), 0);

  const int64_t expected = (kClosePriceRaw - kOpenPriceRaw) * 1000;
  EXPECT_EQ(tracker.getRealizedPnl(SYM).raw(), expected);
}

// Realised PnL is money, not a price: it is a quantity times a price
// difference, so it belongs in the notional type the rest of the engine uses
// for that product (Quantity * Price -> Volume). Typing it as Price lets it
// be compared against, assigned to and added to an actual price without a
// single diagnostic.
//
// needs: Volume PositionTracker::getRealizedPnl(SymbolId) const
// needs: Volume PositionTracker::getTotalRealizedPnl() const
//
// Stated as a type check rather than a direct call so this file still
// compiles against the current signature and fails at run time, the way the
// rest of the suite does. Every other assertion in this file goes through
// raw() and is unaffected by the change.
TEST(PositionTrackerFixedPoint, RealizedPnlIsMoneyTyped)
{
  using PerSymbol = decltype(std::declval<const PositionTracker&>().getRealizedPnl(SYM));
  using Total = decltype(std::declval<const PositionTracker&>().getTotalRealizedPnl());

  EXPECT_TRUE((std::is_same_v<Volume, std::remove_cvref_t<PerSymbol>>))
      << "getRealizedPnl returns a price type; realised PnL is a notional";
  EXPECT_TRUE((std::is_same_v<Volume, std::remove_cvref_t<Total>>))
      << "getTotalRealizedPnl returns a price type; realised PnL is a notional";
}

// Control: the same arithmetic at a magnitude a double still holds exactly.
// Green before the fix and after it -- if this one ever goes red the change
// broke plain position keeping, not just the exactness.
TEST(PositionTrackerFixedPoint, ModestPriceRoundTripIsExactToday)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  fill(tracker, Side::BUY, 10'000'000'000, 100'000'000);           // 100.0 x 1.0
  fill(tracker, Side::BUY, 10'200'000'000, 100'000'000);           // 102.0 x 1.0
  EXPECT_EQ(tracker.getAvgEntryPrice(SYM).raw(), 10'100'000'000);  // 101.0

  fill(tracker, Side::SELL, 11'000'000'000, 200'000'000);  // 110.0 x 2.0
  EXPECT_EQ(tracker.getPosition(SYM).raw(), 0);
  EXPECT_EQ(tracker.getRealizedPnl(SYM).raw(), 1'800'000'000);  // 18.0
}
