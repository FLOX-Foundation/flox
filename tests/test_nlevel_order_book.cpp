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

using namespace flox;

class NLevelOrderBookTest : public ::testing::Test
{
 protected:
  NLevelOrderBook<> book{Price::fromDouble(0.1)};
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

TEST_F(NLevelOrderBookTest, AppliesSnapshotCorrectly)
{
  auto update = makeSnapshot({{Price::fromDouble(100.0), Quantity::fromDouble(2.0)},
                              {Price::fromDouble(99.0), Quantity::fromDouble(1.0)}},
                             {{Price::fromDouble(101.0), Quantity::fromDouble(1.5)},
                              {Price::fromDouble(102.0), Quantity::fromDouble(3.0)}});
  book.applyBookUpdate(*update);

  EXPECT_EQ(book.bestBid(), Price::fromDouble(100.0));
  EXPECT_EQ(book.bestAsk(), Price::fromDouble(101.0));

  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(2.0));
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(99.0)), Quantity::fromDouble(1.0));
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(101.0)), Quantity::fromDouble(1.5));
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(102.0)), Quantity::fromDouble(3.0));
}

TEST_F(NLevelOrderBookTest, AppliesDeltaCorrectly)
{
  auto snap = makeSnapshot({{Price::fromDouble(100.0), Quantity::fromDouble(1.0)}},
                           {{Price::fromDouble(101.0), Quantity::fromDouble(2.0)}});
  book.applyBookUpdate(*snap);

  auto delta = makeDelta({{Price::fromDouble(100.0), Quantity::fromDouble(0.0)},
                          {Price::fromDouble(99.0), Quantity::fromDouble(1.5)}},
                         {{Price::fromDouble(101.0), Quantity::fromDouble(3.0)}});
  book.applyBookUpdate(*delta);

  EXPECT_EQ(book.bestBid(), Price::fromDouble(99.0));
  EXPECT_EQ(book.bestAsk(), Price::fromDouble(101.0));

  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(99.0)), Quantity::fromDouble(1.5));
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(0.0));
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(101.0)), Quantity::fromDouble(3.0));
}

TEST_F(NLevelOrderBookTest, HandlesEmptyBook)
{
  EXPECT_EQ(book.bestBid(), std::nullopt);
  EXPECT_EQ(book.bestAsk(), std::nullopt);
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(123.0)), Quantity::fromDouble(0.0));
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(123.0)), Quantity::fromDouble(0.0));
}
