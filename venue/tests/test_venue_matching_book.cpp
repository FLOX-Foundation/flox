/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-venue/matching_book.h"
#include "flox/book/ladder_book.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

RestingOrder ord(OrderId id, Side s, double p, double q)
{
  return RestingOrder{.id = id, .accountId = 1, .price = px(p), .leaves = qty(q), .side = s};
}

LadderBook::Config ladderCfg()
{
  return LadderBook::Config{
      .basePriceRaw = 0, .tickRaw = px(0.01).raw(), .numLevels = 20000, .maxOrders = 1024};
}

// Compare top-of-book + a crossing-depth probe between the two implementations.
template <class A, class B>
void expectAgree(const A& a, const B& b)
{
  ASSERT_EQ(a.bestBid().has_value(), b.bestBid().has_value());
  ASSERT_EQ(a.bestAsk().has_value(), b.bestAsk().has_value());
  if (a.bestBid().has_value())
  {
    EXPECT_EQ(a.bestBid().value().raw(), b.bestBid().value().raw());
  }
  if (a.bestAsk().has_value())
  {
    EXPECT_EQ(a.bestAsk().value().raw(), b.bestAsk().value().raw());
  }
  EXPECT_EQ(a.availableWithin(Side::BUY, px(200), false).raw(),
            b.availableWithin(Side::BUY, px(200), false).raw());
  EXPECT_EQ(a.availableWithin(Side::SELL, px(1), false).raw(),
            b.availableWithin(Side::SELL, px(1), false).raw());
}
}  // namespace

// The map-reference MatchingBook and the O(1) LadderBook must be observationally
// identical across add / cancel / reduce / fillBest -- the property the venue
// relies on and differentially fuzzes. This is the in-flox equivalence smoke.
TEST(MatchingBook, LadderAndMapAgreeAcrossOps)
{
  MatchingBook m;
  LadderBook l(ladderCfg());

  // Build both sides across several levels.
  for (auto& o : {ord(1, Side::SELL, 101, 5), ord(2, Side::SELL, 102, 3),
                  ord(3, Side::SELL, 101, 2), ord(4, Side::BUY, 99, 4),
                  ord(5, Side::BUY, 98, 6), ord(6, Side::BUY, 99, 1)})
  {
    m.addResting(o.side, o);
    l.addResting(o.side, o);
  }
  EXPECT_TRUE(m.bestBid().has_value() && m.bestBid().value() == px(99));
  EXPECT_TRUE(m.bestAsk().has_value() && m.bestAsk().value() == px(101));
  expectAgree(m, l);

  // Partial fill of the best ask (101 has 5+2=7 across ids 1,3 FIFO).
  m.fillBest(Side::SELL, qty(6));
  l.fillBest(Side::SELL, qty(6));
  expectAgree(m, l);

  // Cancel a bid at the top level.
  m.cancel(4);
  l.cancel(4);
  expectAgree(m, l);

  // Reduce a resting order in place.
  m.reduce(5, qty(2));
  l.reduce(5, qty(2));
  expectAgree(m, l);

  // Drain the remaining asks by cancelling them; the ask side empties in both.
  for (OrderId id : {OrderId{1}, OrderId{2}, OrderId{3}})
  {
    m.cancel(id);
    l.cancel(id);
  }
  EXPECT_FALSE(m.bestAsk().has_value());
  EXPECT_FALSE(l.bestAsk().has_value());
  expectAgree(m, l);
}

TEST(MatchingBook, IcebergHiddenReserveIsRealDepth)
{
  MatchingBook m;
  LadderBook l(ladderCfg());
  // Iceberg sell: display 2, hide 8 (total 10) at 100.
  RestingOrder ice{.id = 1, .accountId = 1, .price = px(100), .leaves = qty(2), .side = Side::SELL, .hidden = qty(8), .peak = qty(2)};
  m.addResting(Side::SELL, ice);
  l.addResting(Side::SELL, ice);
  // availableWithin counts hidden reserve as real crossing liquidity (10, not 2).
  EXPECT_EQ(m.availableWithin(Side::BUY, px(100), false).raw(), qty(10).raw());
  EXPECT_EQ(l.availableWithin(Side::BUY, px(100), false).raw(), qty(10).raw());
}
