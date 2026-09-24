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
