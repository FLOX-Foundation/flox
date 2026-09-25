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
