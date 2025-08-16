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

static inline void ExpectPairNear(const std::pair<double, double>& got,
                                  double expFilled, double expNotional,
                                  double eps = 1e-9)
{
  EXPECT_NEAR(got.first, expFilled, eps);
  EXPECT_NEAR(got.second, expNotional, eps);
}

TEST_F(NLevelOrderBookTest, ConsumeAsks_Basic)
{
  auto up = makeSnapshot(
      {},
      {{Price::fromDouble(100.0), Quantity::fromDouble(1.0)},
       {Price::fromDouble(100.1), Quantity::fromDouble(2.0)},
       {Price::fromDouble(100.2), Quantity::fromDouble(3.0)}});

  book.applyBookUpdate(*up);

  ExpectPairNear(book.consumeAsks(0.0), 0.0, 0.0);
  ExpectPairNear(book.consumeAsks(1.0), 1.0, 100.0);
  ExpectPairNear(book.consumeAsks(2.5), 2.5, 250.15);
  ExpectPairNear(book.consumeAsks(10.0), 6.0, 600.8);
}

TEST_F(NLevelOrderBookTest, ConsumeBids_Basic)
{
  auto up = makeSnapshot(
      {{Price::fromDouble(100.0), Quantity::fromDouble(1.0)},
       {Price::fromDouble(99.9), Quantity::fromDouble(2.0)},
       {Price::fromDouble(99.8), Quantity::fromDouble(3.0)}},
      {});

  book.applyBookUpdate(*up);

  ExpectPairNear(book.consumeBids(2.5), 2.5, 249.85);
  ExpectPairNear(book.consumeBids(10.0), 6.0, 599.2);
}

TEST_F(NLevelOrderBookTest, ConsumeAsks_WithHoles)
{
  auto up = makeSnapshot(
      {},
      {{Price::fromDouble(100.0), Quantity::fromDouble(0.0)},
       {Price::fromDouble(100.1), Quantity::fromDouble(2.0)},
       {Price::fromDouble(100.2), Quantity::fromDouble(0.0)},
       {Price::fromDouble(100.3), Quantity::fromDouble(3.0)}});

  book.applyBookUpdate(*up);

  ExpectPairNear(book.consumeAsks(2.0), 2.0, 200.2);
  ExpectPairNear(book.consumeAsks(4.0), 4.0, 400.8);
  ExpectPairNear(book.consumeAsks(10.0), 5.0, 501.1);
}

TEST_F(NLevelOrderBookTest, ConsumeBids_WithHoles)
{
  auto up = makeSnapshot(
      {{Price::fromDouble(100.0), Quantity::fromDouble(0.0)},
       {Price::fromDouble(99.9), Quantity::fromDouble(2.0)},
       {Price::fromDouble(99.8), Quantity::fromDouble(0.0)},
       {Price::fromDouble(99.7), Quantity::fromDouble(3.0)}},
      {});

  book.applyBookUpdate(*up);

  ExpectPairNear(book.consumeBids(3.0), 3.0, 299.5);
  ExpectPairNear(book.consumeBids(10.0), 5.0, 498.9);
}

TEST_F(NLevelOrderBookTest, Consume_EmptyBook)
{
  auto up = makeSnapshot({}, {});
  book.applyBookUpdate(*up);

  ExpectPairNear(book.consumeAsks(5.0), 0.0, 0.0);
  ExpectPairNear(book.consumeBids(5.0), 0.0, 0.0);
}

TEST_F(NLevelOrderBookTest, Consume_IsConstDoesNotMutate)
{
  auto up = makeSnapshot(
      {{Price::fromDouble(100.0), Quantity::fromDouble(1.0)},
       {Price::fromDouble(99.9), Quantity::fromDouble(2.0)}},
      {{Price::fromDouble(100.1), Quantity::fromDouble(2.0)},
       {Price::fromDouble(100.2), Quantity::fromDouble(3.0)}});

  book.applyBookUpdate(*up);

  auto r1 = book.consumeAsks(3.5);
  auto r2 = book.consumeAsks(3.5);
  ExpectPairNear(r1, r2.first, r2.second);

  auto b1 = book.consumeBids(2.25);
  auto b2 = book.consumeBids(2.25);
  ExpectPairNear(b1, b2.first, b2.second);

  EXPECT_EQ(book.bestAsk(), Price::fromDouble(100.1));
  EXPECT_EQ(book.bestBid(), Price::fromDouble(100.0));
}
