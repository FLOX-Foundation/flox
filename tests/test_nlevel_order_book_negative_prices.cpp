/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"
#include "flox/common.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

using namespace flox;

namespace
{

// Standalone event builder: these cases need their own tick size, so they
// cannot share the pooled fixture in test_nlevel_order_book.cpp.
class Update
{
 public:
  explicit Update(BookUpdateType type) : _res(_buf, sizeof(_buf)), _ev(&_res)
  {
    _ev.update.type = type;
  }

  Update& bid(double price, double qty)
  {
    _ev.update.bids.push_back({Price::fromDouble(price), Quantity::fromDouble(qty)});
    return *this;
  }

  Update& ask(double price, double qty)
  {
    _ev.update.asks.push_back({Price::fromDouble(price), Quantity::fromDouble(qty)});
    return *this;
  }

  const BookUpdateEvent& event() const { return _ev; }

 private:
  std::byte _buf[65536];
  std::pmr::monotonic_buffer_resource _res;
  BookUpdateEvent _ev;
};

Update snapshot()
{
  return Update{BookUpdateType::SNAPSHOT};
}

Update delta()
{
  return Update{BookUpdateType::DELTA};
}

}  // namespace

// A negative price is not an error condition. WTI settled at -37.63 in April
// 2020, day-ahead power clears below zero most windy afternoons, and a calendar
// spread is negative whenever the market is in contango. The book stores those
// levels correctly -- getBidLevels and bidAtPrice return them -- but bestBid and
// bestAsk read the cached tick index and treat every negative tick as "this side
// is empty", so the quote that is actually in the book is never reported.
//
// The expected price is read back out of the level list rather than hard-coded,
// because how a price between two ticks is snapped is being changed elsewhere;
// what is pinned here is that the best quote agrees with the level the book
// holds, whichever tick that turns out to be.
TEST(NLevelOrderBookNegativePrices, BestQuotesReportTheStoredNegativeLevels)
{
  NLevelOrderBook<> book{Price::fromDouble(1.0)};
  book.applyBookUpdate(snapshot().bid(-100.4, 3.0).ask(-99.6, 2.0).event());

  const auto bids = book.getBidLevels(1);
  const auto asks = book.getAskLevels(1);
  ASSERT_EQ(bids.size(), 1u) << "the negative bid was not stored at all";
  ASSERT_EQ(asks.size(), 1u) << "the negative ask was not stored at all";
  ASSERT_LT(bids[0].price.raw(), 0) << "the stored bid is not at a negative price";
  ASSERT_LT(asks[0].price.raw(), 0) << "the stored ask is not at a negative price";

  ASSERT_TRUE(book.bestBid().has_value())
      << "bestBid reports no bid while the book holds one at "
      << bids[0].price.toDouble();
  ASSERT_TRUE(book.bestAsk().has_value())
      << "bestAsk reports no ask while the book holds one at "
      << asks[0].price.toDouble();

  EXPECT_EQ(book.bestBid()->raw(), bids[0].price.raw());
  EXPECT_EQ(book.bestAsk()->raw(), asks[0].price.raw());
  EXPECT_EQ(book.bidAtPrice(*book.bestBid()).raw(), bids[0].quantity.raw());
  EXPECT_EQ(book.askAtPrice(*book.bestAsk()).raw(), asks[0].quantity.raw());
}

// The two states a caller has to tell apart: a side holding a negative quote,
// and a side holding nothing. Today they answer identically.
TEST(NLevelOrderBookNegativePrices, NegativeBidIsNotTheSameAsAnAbsentBid)
{
  NLevelOrderBook<> book{Price::fromDouble(1.0)};
  book.applyBookUpdate(snapshot().bid(-100.0, 1.0).event());

  ASSERT_TRUE(book.bestBid().has_value()) << "a bid at -100.0 is reported as no bid";
  EXPECT_EQ(book.bestBid()->raw(), Price::fromDouble(-100.0).raw());
  EXPECT_FALSE(book.bestAsk().has_value()) << "an ask appeared on a book that has none";
}

TEST(NLevelOrderBookNegativePrices, NegativeAskIsNotTheSameAsAnAbsentAsk)
{
  NLevelOrderBook<> book{Price::fromDouble(1.0)};
  book.applyBookUpdate(snapshot().ask(-99.0, 1.0).event());

  ASSERT_TRUE(book.bestAsk().has_value()) << "an ask at -99.0 is reported as no ask";
  EXPECT_EQ(book.bestAsk()->raw(), Price::fromDouble(-99.0).raw());
  EXPECT_FALSE(book.bestBid().has_value()) << "a bid appeared on a book that has none";
}

// Green control: absence still reads as absence, before and after a book has
// held quotes. Whatever replaces the sentinel must keep this.
TEST(NLevelOrderBookNegativePrices, AnEmptySideStillReportsNoQuote)
{
  NLevelOrderBook<> book{Price::fromDouble(1.0)};
  EXPECT_FALSE(book.bestBid().has_value());
  EXPECT_FALSE(book.bestAsk().has_value());
  EXPECT_FALSE(book.mid().has_value());
  EXPECT_FALSE(book.spread().has_value());
  EXPECT_FALSE(book.isCrossed());

  book.applyBookUpdate(snapshot().bid(-100.0, 1.0).ask(-99.0, 1.0).event());
  book.clear();

  EXPECT_FALSE(book.bestBid().has_value()) << "clear() left a bid behind";
  EXPECT_FALSE(book.bestAsk().has_value()) << "clear() left an ask behind";
  EXPECT_FALSE(book.mid().has_value());
  EXPECT_FALSE(book.spread().has_value());

  // An empty snapshot is the venue saying the book is gone, not a quote at a
  // negative price.
  book.applyBookUpdate(snapshot().bid(-100.0, 1.0).ask(-99.0, 1.0).event());
  book.applyBookUpdate(snapshot().event());
  EXPECT_FALSE(book.bestBid().has_value());
  EXPECT_FALSE(book.bestAsk().has_value());
}

// Green control: nothing about positive prices changes.
TEST(NLevelOrderBookNegativePrices, PositiveQuotesAreUnchanged)
{
  NLevelOrderBook<> book{Price::fromDouble(1.0)};
  book.applyBookUpdate(snapshot().bid(100.0, 2.0).bid(99.0, 1.0).ask(101.0, 1.5).event());

  ASSERT_TRUE(book.bestBid().has_value());
  ASSERT_TRUE(book.bestAsk().has_value());
  EXPECT_EQ(book.bestBid()->raw(), Price::fromDouble(100.0).raw());
  EXPECT_EQ(book.bestAsk()->raw(), Price::fromDouble(101.0).raw());

  ASSERT_TRUE(book.mid().has_value());
  ASSERT_TRUE(book.spread().has_value());
  EXPECT_EQ(book.mid()->raw(), Price::fromDouble(100.5).raw());
  EXPECT_EQ(book.spread()->raw(), Price::fromDouble(1.0).raw());
  EXPECT_FALSE(book.isCrossed());

  // Zero is a price like any other, and the one the raw C accessors spend as a
  // sentinel. The C++ side must not join them.
  NLevelOrderBook<> atZero{Price::fromDouble(1.0)};
  atZero.applyBookUpdate(snapshot().bid(0.0, 1.0).ask(1.0, 1.0).event());
  ASSERT_TRUE(atZero.bestBid().has_value()) << "a bid at 0.0 is reported as no bid";
  EXPECT_EQ(atZero.bestBid()->raw(), 0);
}

// The tick window follows the market, so a market that trades below zero is
// anchored entirely below zero: every index in the array maps to a negative
// tick, and every quote the book can hold is invisible to bestBid/bestAsk.
TEST(NLevelOrderBookNegativePrices, AWindowAnchoredBelowZeroReportsItsBestQuotes)
{
  NLevelOrderBook<> book{Price::fromDouble(1.0)};
  book.applyBookUpdate(snapshot()
                           .bid(-5001.0, 1.0)
                           .bid(-5002.0, 2.0)
                           .ask(-4999.0, 1.0)
                           .ask(-4998.0, 2.0)
                           .event());

  ASSERT_EQ(book.getBidLevels(2).size(), 2u);
  ASSERT_EQ(book.getAskLevels(2).size(), 2u);

  ASSERT_TRUE(book.bestBid().has_value()) << "no bid on a window anchored below zero";
  ASSERT_TRUE(book.bestAsk().has_value()) << "no ask on a window anchored below zero";
  EXPECT_EQ(book.bestBid()->raw(), Price::fromDouble(-5001.0).raw());
  EXPECT_EQ(book.bestAsk()->raw(), Price::fromDouble(-4999.0).raw());

  // And the cached cursor keeps up with a delta inside that window.
  book.applyBookUpdate(delta().bid(-5000.0, 1.0).ask(-4999.0, 0.0).event());
  ASSERT_TRUE(book.bestBid().has_value());
  ASSERT_TRUE(book.bestAsk().has_value());
  EXPECT_EQ(book.bestBid()->raw(), Price::fromDouble(-5000.0).raw());
  EXPECT_EQ(book.bestAsk()->raw(), Price::fromDouble(-4998.0).raw());

  // Pulling the last bid empties the side; that is a different answer from a
  // negative quote, and it has to stay different.
  book.applyBookUpdate(delta().bid(-5000.0, 0.0).bid(-5001.0, 0.0).bid(-5002.0, 0.0).event());
  EXPECT_FALSE(book.bestBid().has_value());
  EXPECT_TRUE(book.bestAsk().has_value());
}

// mid() and spread() read the same two cached ticks through the same `< 0`
// test, so a negative book has no mid and no spread either. Both quotes sit on
// exact ticks here, so the expected values do not depend on how an off-tick
// price is snapped.
TEST(NLevelOrderBookNegativePrices, MidAndSpreadAreReportedAtNegativePrices)
{
  NLevelOrderBook<> book{Price::fromDouble(1.0)};
  book.applyBookUpdate(snapshot().bid(-101.0, 1.0).ask(-99.0, 1.0).event());

  ASSERT_TRUE(book.spread().has_value()) << "no spread on a book quoted below zero";
  ASSERT_TRUE(book.mid().has_value()) << "no mid on a book quoted below zero";
  EXPECT_EQ(book.spread()->raw(), Price::fromDouble(2.0).raw());
  EXPECT_EQ(book.mid()->raw(), Price::fromDouble(-100.0).raw());
}

// isCrossed() short-circuits on the same sentinel, so a book that is crossed
// below zero -- the state a feed handler is watching for -- reports clean.
TEST(NLevelOrderBookNegativePrices, ACrossedNegativeBookIsReportedCrossed)
{
  NLevelOrderBook<> crossed{Price::fromDouble(1.0)};
  crossed.applyBookUpdate(snapshot().bid(-99.0, 1.0).ask(-101.0, 1.0).event());
  EXPECT_TRUE(crossed.isCrossed()) << "a bid at -99.0 over an ask at -101.0 reads as clean";

  NLevelOrderBook<> clean{Price::fromDouble(1.0)};
  clean.applyBookUpdate(snapshot().bid(-101.0, 1.0).ask(-99.0, 1.0).event());
  EXPECT_FALSE(clean.isCrossed());

  // Green control: the positive case already works and must keep working.
  NLevelOrderBook<> positive{Price::fromDouble(1.0)};
  positive.applyBookUpdate(snapshot().bid(101.0, 1.0).ask(100.0, 1.0).event());
  EXPECT_TRUE(positive.isCrossed());
}

// The boundary isCrossed() is written on: `*bid >= *ask`, which the reference
// documents as "true if best bid >= best ask (crossed/locked market)". A locked
// book -- bid and ask on the same tick, spread zero -- is therefore reported
// crossed, not clean, and that is what is pinned here. The cases above only ever
// place the two sides a tick or more apart, so nothing there tells a `>=` from
// a `>`.
TEST(NLevelOrderBookNegativePrices, ALockedBookIsReportedCrossed)
{
  NLevelOrderBook<> locked{Price::fromDouble(1.0)};
  locked.applyBookUpdate(snapshot().bid(100.0, 1.0).ask(100.0, 1.0).event());

  ASSERT_TRUE(locked.bestBid().has_value());
  ASSERT_TRUE(locked.bestAsk().has_value());
  ASSERT_EQ(locked.bestBid()->raw(), locked.bestAsk()->raw())
      << "the two sides did not land on the same tick, so this is not a locked book";
  ASSERT_TRUE(locked.spread().has_value());
  EXPECT_EQ(locked.spread()->raw(), 0);
  EXPECT_TRUE(locked.isCrossed())
      << "a bid and an ask on the same tick read as a clean book";

  // The same tick below zero, where the sentinel used to hide the state
  // entirely.
  NLevelOrderBook<> lockedBelowZero{Price::fromDouble(1.0)};
  lockedBelowZero.applyBookUpdate(snapshot().bid(-100.0, 1.0).ask(-100.0, 1.0).event());
  ASSERT_TRUE(lockedBelowZero.bestBid().has_value());
  ASSERT_TRUE(lockedBelowZero.bestAsk().has_value());
  ASSERT_EQ(lockedBelowZero.bestBid()->raw(), lockedBelowZero.bestAsk()->raw());
  EXPECT_TRUE(lockedBelowZero.isCrossed())
      << "a bid and an ask on the same negative tick read as a clean book";

  // And locked exactly at zero, the price the C accessors spend as a sentinel.
  NLevelOrderBook<> lockedAtZero{Price::fromDouble(1.0)};
  lockedAtZero.applyBookUpdate(snapshot().bid(0.0, 1.0).ask(0.0, 1.0).event());
  ASSERT_TRUE(lockedAtZero.bestBid().has_value());
  ASSERT_TRUE(lockedAtZero.bestAsk().has_value());
  EXPECT_EQ(lockedAtZero.bestBid()->raw(), 0);
  EXPECT_EQ(lockedAtZero.bestAsk()->raw(), 0);
  EXPECT_TRUE(lockedAtZero.isCrossed());

  // Green control: one tick apart is not crossed, so the assertion above is
  // pinning the boundary rather than making isCrossed() answer true everywhere.
  NLevelOrderBook<> oneTickApart{Price::fromDouble(1.0)};
  oneTickApart.applyBookUpdate(snapshot().bid(99.0, 1.0).ask(100.0, 1.0).event());
  EXPECT_FALSE(oneTickApart.isCrossed());
}

// The delta path rebuilds the cached best cursor from scratch after it moves the
// window (reanchorWithData), and that rebuild is driven by the level index, not
// by the tick, so it finds the right level. Only the tick it writes down is then
// misread. This walks a market from above zero to below it, which is the shape
// the April 2020 WTI tape had, and pins that the quotes survive the move.
TEST(NLevelOrderBookNegativePrices, AMarketWalkingBelowZeroKeepsItsBestQuotes)
{
  NLevelOrderBook<64> book{Price::fromDouble(1.0)};
  book.applyBookUpdate(snapshot().bid(20.0, 1.0).ask(21.0, 1.0).event());
  ASSERT_TRUE(book.bestBid().has_value());
  EXPECT_EQ(book.bestBid()->raw(), Price::fromDouble(20.0).raw());

  // New quotes far below zero: outside the window, so the book re-anchors.
  book.applyBookUpdate(delta().bid(-30.0, 1.0).ask(-29.0, 1.0).event());

  // Read the best quotes here, while both markets are still in the book. The
  // re-anchor rebuilt the cached cursors by walking the whole ladder, and the
  // best bid of a book holding 20.0 and -30.0 is 20.0 -- the highest level, not
  // the lowest one, and not the level that happened to arrive last. Once the
  // stale side is pulled below, only one bid is left and the two answers
  // coincide, so a rebuild running the wrong way would pass unnoticed.
  ASSERT_TRUE(book.bestBid().has_value()) << "the re-anchor emptied the bid side";
  ASSERT_TRUE(book.bestAsk().has_value()) << "the re-anchor emptied the ask side";
  EXPECT_EQ(book.bestBid()->raw(), Price::fromDouble(20.0).raw())
      << "the best bid across the re-anchor is not the highest level in the book";
  EXPECT_EQ(book.bestAsk()->raw(), Price::fromDouble(-29.0).raw())
      << "the best ask across the re-anchor is not the lowest level in the book";
  EXPECT_TRUE(book.isCrossed()) << "a bid at 20.0 over an ask at -29.0 reads as clean";

  // The old ones are pulled, leaving only the negative side of the book.
  book.applyBookUpdate(delta().bid(20.0, 0.0).ask(21.0, 0.0).event());

  const auto bids = book.getBidLevels(4);
  const auto asks = book.getAskLevels(4);
  ASSERT_EQ(bids.size(), 1u) << "the re-anchor dropped the negative bid";
  ASSERT_EQ(asks.size(), 1u) << "the re-anchor dropped the negative ask";
  EXPECT_EQ(bids[0].price.raw(), Price::fromDouble(-30.0).raw());
  EXPECT_EQ(asks[0].price.raw(), Price::fromDouble(-29.0).raw());

  ASSERT_TRUE(book.bestBid().has_value()) << "no bid after the market walked below zero";
  ASSERT_TRUE(book.bestAsk().has_value()) << "no ask after the market walked below zero";
  EXPECT_EQ(book.bestBid()->raw(), Price::fromDouble(-30.0).raw());
  EXPECT_EQ(book.bestAsk()->raw(), Price::fromDouble(-29.0).raw());

  // An odd tick sum, so the half-tick in mid() is taken on a negative total.
  ASSERT_TRUE(book.mid().has_value());
  EXPECT_EQ(book.mid()->raw(), Price::fromDouble(-29.5).raw());
  ASSERT_TRUE(book.spread().has_value());
  EXPECT_EQ(book.spread()->raw(), Price::fromDouble(1.0).raw());
}

// Moving the window throws the cached best-quote cursors away and rebuilds them
// by walking the whole ladder, so the rebuild has to pick the same level the
// incremental path would: the highest bid, the lowest ask. With depth on both
// sides at the moment the window moves, a rebuild running the wrong way down
// the ladder picks the far side of the book -- and the market is then quoted at
// a price behind the touch, which is exactly the read a strategy sizes against.
// The re-anchor here is triggered by a quote far below zero, the move a market
// walking through zero makes.
TEST(NLevelOrderBookNegativePrices, ARebuiltBookStillQuotesItsTouch)
{
  NLevelOrderBook<64> book{Price::fromDouble(1.0)};
  book.applyBookUpdate(
      snapshot().bid(20.0, 1.0).bid(19.0, 2.0).ask(21.0, 1.0).ask(22.0, 2.0).event());
  ASSERT_EQ(book.getBidLevels(4).size(), 2u);
  ASSERT_EQ(book.getAskLevels(4).size(), 2u);

  // Outside the window, so the book re-anchors and rebuilds both cursors while
  // two levels are standing on each side.
  book.applyBookUpdate(delta().bid(-30.0, 1.0).event());

  ASSERT_EQ(book.getBidLevels(4).size(), 3u) << "the re-anchor dropped a bid level";
  ASSERT_EQ(book.getAskLevels(4).size(), 2u) << "the re-anchor dropped an ask level";

  ASSERT_TRUE(book.bestBid().has_value()) << "the rebuild emptied the bid side";
  ASSERT_TRUE(book.bestAsk().has_value()) << "the rebuild emptied the ask side";
  EXPECT_EQ(book.bestBid()->raw(), Price::fromDouble(20.0).raw())
      << "the rebuilt best bid is not the highest level in the book";
  EXPECT_EQ(book.bestAsk()->raw(), Price::fromDouble(21.0).raw())
      << "the rebuilt best ask is not the lowest level in the book";

  // The level walk and the best quote must agree, here as everywhere else.
  EXPECT_EQ(book.getBidLevels(1)[0].price.raw(), book.bestBid()->raw());
  EXPECT_EQ(book.getAskLevels(1)[0].price.raw(), book.bestAsk()->raw());
  ASSERT_TRUE(book.spread().has_value());
  EXPECT_EQ(book.spread()->raw(), Price::fromDouble(1.0).raw());
}

// A snapshot spanning the whole ladder anchors the window on its lowest tick
// (reanchor(): a span of MAX_LEVELS or more sets _baseIndex to minIdx), so the
// best bid lands on ladder index 0 and the best ask on the last slot. Those are
// ordinary levels, but they are also the two indices an emptiness test can
// confuse with an empty side: 0 is the falsy one, and the top slot sits one
// short of the MAX_LEVELS marker that does mean "no side". Every other case in
// this file quotes somewhere in the middle of the window.
TEST(NLevelOrderBookNegativePrices, TheWindowsFloorAndTopSlotAreQuotesLikeAnyOther)
{
  NLevelOrderBook<64> book{Price::fromDouble(1.0)};
  // Ticks -100 and -37: a span of exactly 64 levels, so the window is anchored
  // at -100 and the two quotes take its first and its last slot.
  book.applyBookUpdate(snapshot().bid(-100.0, 3.0).ask(-37.0, 2.0).event());

  ASSERT_EQ(book.getBidLevels(4).size(), 1u) << "the bid on the window's floor was not stored";
  ASSERT_EQ(book.getAskLevels(4).size(), 1u) << "the ask on the window's top slot was not stored";

  ASSERT_TRUE(book.bestBid().has_value())
      << "the bid on the window's floor is reported as no bid";
  ASSERT_TRUE(book.bestAsk().has_value())
      << "the ask on the window's top slot is reported as no ask";
  EXPECT_EQ(book.bestBid()->raw(), Price::fromDouble(-100.0).raw());
  EXPECT_EQ(book.bestAsk()->raw(), Price::fromDouble(-37.0).raw());
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(-100.0)).raw(), Quantity::fromDouble(3.0).raw());
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(-37.0)).raw(), Quantity::fromDouble(2.0).raw());

  ASSERT_TRUE(book.spread().has_value()) << "no spread while both sides are quoted";
  ASSERT_TRUE(book.mid().has_value()) << "no mid while both sides are quoted";
  EXPECT_EQ(book.spread()->raw(), Price::fromDouble(63.0).raw());
  EXPECT_EQ(book.mid()->raw(), Price::fromDouble(-68.5).raw());
  EXPECT_FALSE(book.isCrossed());

  // That these two levels really are the first and the last slot is visible
  // from outside the book: a quote one tick beyond either end does not fit in
  // the window, and the window cannot follow it without evicting the live depth
  // at the other end, so the level is dropped.
  book.applyBookUpdate(delta().bid(-101.0, 1.0).event());
  ASSERT_EQ(book.getBidLevels(4).size(), 1u)
      << "-101.0 fit in the window, so -100.0 was not sitting on its floor";
  EXPECT_EQ(book.bestBid()->raw(), Price::fromDouble(-100.0).raw());

  book.applyBookUpdate(delta().ask(-36.0, 1.0).event());
  ASSERT_EQ(book.getAskLevels(4).size(), 1u)
      << "-36.0 fit in the window, so -37.0 was not sitting on its top slot";
  EXPECT_EQ(book.bestAsk()->raw(), Price::fromDouble(-37.0).raw());
}

// The reference says spread "Returns ask - bid spread"
// (docs/reference/api/book/nlevel_order_book.md:105) and says nothing about
// what that means once the two sides overlap, so what is pinned here is the
// sign that formula produces: a crossed book has a negative spread. It is the
// only reading that keeps spread() and isCrossed() telling the same story, and
// a feed handler watching the touch invert wants the size of the inversion, not
// its magnitude with the sign thrown away -- an absolute value would report a
// crossed book as a wide one.
TEST(NLevelOrderBookNegativePrices, ACrossedBookReportsANegativeSpread)
{
  NLevelOrderBook<> crossed{Price::fromDouble(1.0)};
  crossed.applyBookUpdate(snapshot().bid(101.0, 1.0).ask(99.0, 1.0).event());

  ASSERT_TRUE(crossed.isCrossed());
  ASSERT_TRUE(crossed.spread().has_value());
  EXPECT_LT(crossed.spread()->raw(), 0)
      << "a bid two ticks above the ask reports a non-negative spread";
  EXPECT_EQ(crossed.spread()->raw(), Price::fromDouble(-2.0).raw());
  // The mid stays between the two quotes either way.
  ASSERT_TRUE(crossed.mid().has_value());
  EXPECT_EQ(crossed.mid()->raw(), Price::fromDouble(100.0).raw());

  // The same inversion below zero, where the old sentinel hid it entirely.
  NLevelOrderBook<> crossedBelowZero{Price::fromDouble(1.0)};
  crossedBelowZero.applyBookUpdate(snapshot().bid(-99.0, 1.0).ask(-101.0, 1.0).event());
  ASSERT_TRUE(crossedBelowZero.isCrossed());
  ASSERT_TRUE(crossedBelowZero.spread().has_value());
  EXPECT_EQ(crossedBelowZero.spread()->raw(), Price::fromDouble(-2.0).raw());

  // A locked book sits on the boundary: zero, with no sign to get wrong.
  NLevelOrderBook<> locked{Price::fromDouble(1.0)};
  locked.applyBookUpdate(snapshot().bid(100.0, 1.0).ask(100.0, 1.0).event());
  ASSERT_TRUE(locked.spread().has_value());
  EXPECT_EQ(locked.spread()->raw(), 0);

  // Green control: an ordinary book keeps its positive spread, so the case
  // above pins the sign rather than flipping it everywhere.
  NLevelOrderBook<> clean{Price::fromDouble(1.0)};
  clean.applyBookUpdate(snapshot().bid(99.0, 1.0).ask(101.0, 1.0).event());
  ASSERT_TRUE(clean.spread().has_value());
  EXPECT_EQ(clean.spread()->raw(), Price::fromDouble(2.0).raw());
}
