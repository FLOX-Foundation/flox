/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tick snapping in NLevelOrderBook must be conservative, not nearest.
//
// A quote that does not sit on a tick is stored at a tick index, and reading
// it back returns that tick's price. Rounding to the nearest tick moves an
// ask down by up to half a tick and a bid up by up to half a tick, so the
// book hands the strategy a price better than the venue ever quoted. The
// strategy then sizes against liquidity that is not there.
//
// The rule these tests pin: a stored ask is never below the quoted ask, and a
// stored bid is never above the quoted bid.

#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"
#include "flox/common.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{

class TickRoundingTest : public ::testing::Test
{
 protected:
  using BookUpdatePool = pool::Pool<BookUpdateEvent, 63>;
  BookUpdatePool pool;

  pool::Handle<BookUpdateEvent> makeSnapshot(const std::vector<BookLevel>& bids,
                                             const std::vector<BookLevel>& asks)
  {
    auto opt = pool.acquire();
    assert(opt);
    auto& u = *opt;
    u->update.type = BookUpdateType::SNAPSHOT;
    u->update.bids.assign(bids.begin(), bids.end());
    u->update.asks.assign(asks.begin(), asks.end());
    return std::move(u);
  }

  pool::Handle<BookUpdateEvent> makeDelta(const std::vector<BookLevel>& bids,
                                          const std::vector<BookLevel>& asks)
  {
    auto opt = pool.acquire();
    assert(opt);
    auto& u = *opt;
    u->update.type = BookUpdateType::DELTA;
    u->update.bids.assign(bids.begin(), bids.end());
    u->update.asks.assign(asks.begin(), asks.end());
    return std::move(u);
  }
};

}  // namespace

TEST_F(TickRoundingTest, AskBetweenTicksIsNeverStoredBelowTheQuote)
{
  NLevelOrderBook<512> book{Price::fromDouble(1.0)};

  const Price quotedAsk = Price::fromDouble(100.4);
  auto update = makeSnapshot({{Price::fromDouble(90.0), Quantity::fromDouble(1.0)}},
                             {{quotedAsk, Quantity::fromDouble(1.0)}});
  book.applyBookUpdate(*update);

  auto best = book.bestAsk();
  ASSERT_TRUE(best.has_value());
  EXPECT_GE(best->raw(), quotedAsk.raw())
      << "best ask " << best->raw() << " is better than the quoted " << quotedAsk.raw();
  EXPECT_EQ(best->raw(), Price::fromDouble(101.0).raw());
}

TEST_F(TickRoundingTest, BidBetweenTicksIsNeverStoredAboveTheQuote)
{
  NLevelOrderBook<512> book{Price::fromDouble(1.0)};

  const Price quotedBid = Price::fromDouble(99.6);
  auto update = makeSnapshot({{quotedBid, Quantity::fromDouble(1.0)}},
                             {{Price::fromDouble(110.0), Quantity::fromDouble(1.0)}});
  book.applyBookUpdate(*update);

  auto best = book.bestBid();
  ASSERT_TRUE(best.has_value());
  EXPECT_LE(best->raw(), quotedBid.raw())
      << "best bid " << best->raw() << " is better than the quoted " << quotedBid.raw();
  EXPECT_EQ(best->raw(), Price::fromDouble(99.0).raw());
}

// A cent tick with a sub-cent quote: the shape an exchange that publishes more
// precision than its own tick actually produces.
TEST_F(TickRoundingTest, SubTickQuotesOnACentTickStayConservative)
{
  NLevelOrderBook<512> book{Price::fromDouble(0.01)};

  const Price quotedBid = Price::fromDouble(99.995);
  const Price quotedAsk = Price::fromDouble(100.004999);
  auto update = makeSnapshot({{quotedBid, Quantity::fromDouble(1.0)}},
                             {{quotedAsk, Quantity::fromDouble(1.0)}});
  book.applyBookUpdate(*update);

  auto bid = book.bestBid();
  auto ask = book.bestAsk();
  ASSERT_TRUE(bid.has_value());
  ASSERT_TRUE(ask.has_value());

  EXPECT_LE(bid->raw(), quotedBid.raw());
  EXPECT_GE(ask->raw(), quotedAsk.raw());
  EXPECT_EQ(bid->raw(), Price::fromDouble(99.99).raw());
  EXPECT_EQ(ask->raw(), Price::fromDouble(100.01).raw());
}

// The whole half-tick band, not one point in it: every off-tick offset has to
// snap the same way, and nothing in the band may cross the quote.
TEST_F(TickRoundingTest, EveryOffsetInsideATickSnapsAwayFromTheQuote)
{
  const int64_t tickRaw = Price::fromDouble(1.0).raw();

  for (int64_t offset = 1; offset < tickRaw; offset += tickRaw / 16)
  {
    NLevelOrderBook<512> book{Price::fromDouble(1.0)};

    const Price quotedAsk = Price::fromRaw(Price::fromDouble(200.0).raw() + offset);
    const Price quotedBid = Price::fromRaw(Price::fromDouble(100.0).raw() + offset);
    auto update = makeSnapshot({{quotedBid, Quantity::fromDouble(1.0)}},
                               {{quotedAsk, Quantity::fromDouble(1.0)}});
    book.applyBookUpdate(*update);

    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    ASSERT_TRUE(bid.has_value()) << "offset " << offset;
    ASSERT_TRUE(ask.has_value()) << "offset " << offset;

    EXPECT_GE(ask->raw(), quotedAsk.raw()) << "offset " << offset;
    EXPECT_LE(bid->raw(), quotedBid.raw()) << "offset " << offset;
    EXPECT_EQ(ask->raw(), Price::fromDouble(201.0).raw()) << "offset " << offset;
    EXPECT_EQ(bid->raw(), Price::fromDouble(100.0).raw()) << "offset " << offset;
  }
}

// The snapshot path sizes its tick window from the same snap the write loop
// below it uses. Feeding that scan the wrong side moves each bound by one
// tick, which is invisible until the window is tight: here the quotes span
// exactly MAX_LEVELS ticks, so the window has no slack at all and a bound that
// is off by one puts the ask one tick past the end of the book.
TEST_F(TickRoundingTest, SnapshotWindowIsSizedFromTheSideAwareSnap)
{
  NLevelOrderBook<512> book{Price::fromDouble(1.0)};

  // floor(100.5) = 100 and ceil(610.5) = 611: 512 ticks inclusive, exactly the
  // level count, so the window anchors at 100 and the ask sits on its last
  // slot.
  auto update = makeSnapshot({{Price::fromDouble(100.5), Quantity::fromDouble(1.0)}},
                             {{Price::fromDouble(610.5), Quantity::fromDouble(2.0)}});
  book.applyBookUpdate(*update);

  auto bid = book.bestBid();
  auto ask = book.bestAsk();
  ASSERT_TRUE(bid.has_value());
  ASSERT_TRUE(ask.has_value()) << "the ask fell outside a window sized the wrong way";
  EXPECT_EQ(bid->raw(), Price::fromDouble(100.0).raw());
  EXPECT_EQ(ask->raw(), Price::fromDouble(611.0).raw());
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(100.5)).raw(), Quantity::fromDouble(1.0).raw());
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(610.5)).raw(), Quantity::fromDouble(2.0).raw());
}

// A price is looked up at the tick it was stored at, so the query has to snap
// the way the write did. Rounding a query to the nearest tick reads the slot
// next to the one holding the level -- which is empty, so the book answers
// "no size here" about a level it is holding.
TEST_F(TickRoundingTest, PriceQueriesSnapTheSameWayTheWriteDid)
{
  NLevelOrderBook<512> book{Price::fromDouble(1.0)};

  auto update = makeSnapshot({{Price::fromDouble(99.6), Quantity::fromDouble(3.0)}},
                             {{Price::fromDouble(100.4), Quantity::fromDouble(4.0)}});
  book.applyBookUpdate(*update);

  // Asked at the quoted price, the book reports the size it stored.
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(99.6)).raw(), Quantity::fromDouble(3.0).raw());
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(100.4)).raw(), Quantity::fromDouble(4.0).raw());

  // Asked at the tick each price is nearest to, it reports nothing: that slot
  // was never written.
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(100.0)).raw(), 0);
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(100.0)).raw(), 0);

  // And the ticks the levels really landed on answer as well.
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(99.0)).raw(), Quantity::fromDouble(3.0).raw());
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(101.0)).raw(), Quantity::fromDouble(4.0).raw());
}

// The delta path has its own scan, and it decides whether the window has to
// move before the level is written. A level whose conservative snap lands one
// tick past the end of the window needs that move; a scan that snaps the other
// way concludes the level already fits, the window stays where it is, and the
// write silently drops it.
TEST_F(TickRoundingTest, DeltaReanchorsOnTheSideAwareSnap)
{
  NLevelOrderBook<512> book{Price::fromDouble(1.0)};

  // Bids only, near enough to the middle of the fresh window that the snapshot
  // leaves the anchor at tick zero: the window is ticks 0..511.
  auto snapshot = makeSnapshot({{Price::fromDouble(300.0), Quantity::fromDouble(1.0)}}, {});
  book.applyBookUpdate(*snapshot);
  ASSERT_TRUE(book.bestBid().has_value());
  ASSERT_FALSE(book.bestAsk().has_value());

  // ceil(511.5) = 512, one past the last slot; floor(511.5) = 511, the last
  // slot. The book has to re-anchor for this one.
  auto delta = makeDelta({}, {{Price::fromDouble(511.5), Quantity::fromDouble(2.0)}});
  book.applyBookUpdate(*delta);

  auto ask = book.bestAsk();
  ASSERT_TRUE(ask.has_value()) << "the delta level was dropped instead of re-anchored";
  EXPECT_EQ(ask->raw(), Price::fromDouble(512.0).raw());
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(511.5)).raw(), Quantity::fromDouble(2.0).raw());

  auto bid = book.bestBid();
  ASSERT_TRUE(bid.has_value()) << "the re-anchor lost the side that was already there";
  EXPECT_EQ(bid->raw(), Price::fromDouble(300.0).raw());
}

// Quotes below zero. A negative price is not a malformed one -- crude settled
// there in April 2020, day-ahead power does it regularly, and a calendar
// spread is quoted negative most of its life -- and it is the only thing that
// reaches the negative half of the tick division: ticks() is handed the price
// itself, so where the window is anchored never makes the argument negative.
// Snapping away from the quote there means away from zero for a bid and
// toward zero for an ask, the opposite of what truncating integer division
// does on its own: -100.4 truncates to -100, a price better than the bid
// quoted.
//
// Read through the level accessors rather than bestBid()/bestAsk(): those two
// report a tick index below zero as "no book" (the `t < 0` sentinel at
// nlevel_order_book.h:454), so they cannot express a negative best quote at
// all. That is a separate limit of the accessors, not of the snapping these
// tests are about.
TEST_F(TickRoundingTest, NegativeQuotesSnapAwayFromTheQuote)
{
  NLevelOrderBook<512> book{Price::fromDouble(1.0)};

  const Price quotedBid = Price::fromDouble(-100.4);
  const Price quotedAsk = Price::fromDouble(-99.6);
  auto update = makeSnapshot({{quotedBid, Quantity::fromDouble(1.0)}},
                             {{quotedAsk, Quantity::fromDouble(2.0)}});
  book.applyBookUpdate(*update);

  const auto bids = book.getBidLevels(1);
  const auto asks = book.getAskLevels(1);
  ASSERT_EQ(bids.size(), 1u);
  ASSERT_EQ(asks.size(), 1u);

  EXPECT_LE(bids[0].price.raw(), quotedBid.raw())
      << "bid " << bids[0].price.raw() << " is better than the quoted " << quotedBid.raw();
  EXPECT_GE(asks[0].price.raw(), quotedAsk.raw())
      << "ask " << asks[0].price.raw() << " is better than the quoted " << quotedAsk.raw();

  // -100.4 floors to -101, not to the -100 that truncation toward zero gives.
  EXPECT_EQ(bids[0].price.raw(), Price::fromDouble(-101.0).raw());
  EXPECT_EQ(asks[0].price.raw(), Price::fromDouble(-99.0).raw());

  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(-101.0)).raw(), Quantity::fromDouble(1.0).raw());
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(-100.0)).raw(), 0);
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(-99.0)).raw(), Quantity::fromDouble(2.0).raw());
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(-100.0)).raw(), 0);

  // And the quoted price itself reads the level it was stored on.
  EXPECT_EQ(book.bidAtPrice(quotedBid).raw(), Quantity::fromDouble(1.0).raw());
  EXPECT_EQ(book.askAtPrice(quotedAsk).raw(), Quantity::fromDouble(2.0).raw());
}

// The same band swept below zero, so the whole negative half is covered and
// not one point in it.
TEST_F(TickRoundingTest, EveryOffsetInsideATickSnapsAwayFromTheQuoteBelowZero)
{
  const int64_t tickRaw = Price::fromDouble(1.0).raw();

  for (int64_t offset = 1; offset < tickRaw; offset += tickRaw / 16)
  {
    NLevelOrderBook<512> book{Price::fromDouble(1.0)};

    // -200 + offset and -100 + offset: both land strictly between two ticks.
    const Price quotedBid = Price::fromRaw(Price::fromDouble(-200.0).raw() + offset);
    const Price quotedAsk = Price::fromRaw(Price::fromDouble(-100.0).raw() + offset);
    auto update = makeSnapshot({{quotedBid, Quantity::fromDouble(1.0)}},
                               {{quotedAsk, Quantity::fromDouble(1.0)}});
    book.applyBookUpdate(*update);

    const auto bids = book.getBidLevels(1);
    const auto asks = book.getAskLevels(1);
    ASSERT_EQ(bids.size(), 1u) << "offset " << offset;
    ASSERT_EQ(asks.size(), 1u) << "offset " << offset;

    EXPECT_LE(bids[0].price.raw(), quotedBid.raw()) << "offset " << offset;
    EXPECT_GE(asks[0].price.raw(), quotedAsk.raw()) << "offset " << offset;
    EXPECT_EQ(bids[0].price.raw(), Price::fromDouble(-200.0).raw()) << "offset " << offset;
    EXPECT_EQ(asks[0].price.raw(), Price::fromDouble(-99.0).raw()) << "offset " << offset;
  }
}

// Where the window happens to sit is not part of the snap: the tick a quote
// belongs to is a property of the price and the tick size, and the anchor is
// subtracted afterwards. Pinned with the anchor above the quote, the one
// arrangement the cases above never produce.
TEST_F(TickRoundingTest, SnapIsIndependentOfWhereTheWindowIsAnchored)
{
  NLevelOrderBook<512> book{Price::fromDouble(1.0)};

  // Far enough up that the snapshot re-anchors instead of keeping the fresh
  // window: the anchor lands at tick 744, well above the quote added next.
  auto snapshot = makeSnapshot({{Price::fromDouble(1000.0), Quantity::fromDouble(1.0)}},
                               {{Price::fromDouble(1001.0), Quantity::fromDouble(1.0)}});
  book.applyBookUpdate(*snapshot);

  auto delta = makeDelta({{Price::fromDouble(800.6), Quantity::fromDouble(2.0)}}, {});
  book.applyBookUpdate(*delta);

  // 800.6 is nearest to tick 801 and floors to 800; the level is at 800.
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(800.6)).raw(), Quantity::fromDouble(2.0).raw());
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(801.0)).raw(), 0);

  const auto levels = book.getBidLevels(2);
  ASSERT_EQ(levels.size(), 2u);
  EXPECT_EQ(levels[0].price.raw(), Price::fromDouble(1000.0).raw());
  EXPECT_EQ(levels[1].price.raw(), Price::fromDouble(800.0).raw());
}

// Control: prices that already sit on a tick are unchanged by any rounding
// rule. Green before the fix and after it.
TEST_F(TickRoundingTest, OnTickQuotesAreUnchanged)
{
  NLevelOrderBook<512> book{Price::fromDouble(0.5)};

  auto update = makeSnapshot({{Price::fromDouble(99.5), Quantity::fromDouble(1.0)},
                              {Price::fromDouble(99.0), Quantity::fromDouble(2.0)}},
                             {{Price::fromDouble(100.5), Quantity::fromDouble(1.0)},
                              {Price::fromDouble(101.0), Quantity::fromDouble(2.0)}});
  book.applyBookUpdate(*update);

  auto bid = book.bestBid();
  auto ask = book.bestAsk();
  ASSERT_TRUE(bid.has_value());
  ASSERT_TRUE(ask.has_value());
  EXPECT_EQ(bid->raw(), Price::fromDouble(99.5).raw());
  EXPECT_EQ(ask->raw(), Price::fromDouble(100.5).raw());
}
