/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The id index of LadderBook, asked about the two ways it used to lose orders.
//
// The index is open-addressed over a table sized past twice the node pool.
// Deletion used to write a tombstone and nothing ever reclaimed one, which
// cost two things, not one:
//
//   - probe chains that grew with the orders the venue had EVER seen. That is
//     what test_venue_ladder_book_contract measures.
//   - saturation. Once the last empty slot in the table had become a
//     tombstone, insertSlot's probe loop ran off its end without writing
//     anything and returned. The order was on its price level, matching and
//     printing, and contains(), find() and cancel() all denied it existed --
//     it could never be taken off the book again. Measured on the previous
//     code at pool sizes 2^12 and 2^13: 64 resting orders out of 64 lost.
//
// This file is about the second one. It states the invariant that makes it
// impossible rather than the symptom: the index never saturates, and if it
// ever did, addResting refuses the order instead of taking it half-way on.

#include "flox-venue/matching_engine.h"
#include "flox/book/ladder_book.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

RestingOrder resting(OrderId id, Side s, double p, double q)
{
  return RestingOrder{.id = id, .accountId = 7, .price = px(p), .leaves = qty(q), .side = s};
}

LadderBook::Config cfgFor(int32_t maxOrders)
{
  return LadderBook::Config{
      .basePriceRaw = 0, .tickRaw = px(0.01).raw(), .numLevels = 4096, .maxOrders = maxOrders};
}

}  // namespace

// The pool sizes at which the previous index saturated, driven past the point
// where every slot had been touched. A resting order is still a resting order
// afterwards: the index answers for all of them, and the book gives them all
// back.
TEST(LadderBookIdIndex, ChurnPastTheTableSizeLosesNoRestingOrder)
{
  for (const int32_t pool : {1 << 12, 1 << 13})
  {
    LadderBook book(cfgFor(pool));

    // 128 pools' worth of lifecycles: that is where the previous index went
    // from "slower" to "lossy" at both of these sizes (measured -- 0 of 64
    // resting orders lost at 64x, 64 of 64 at 128x, because saturation is
    // reached only once the last empty slot in the table has been claimed by
    // a tombstone). The fixed index does this churn at its fresh cost, so the
    // loop that used to take seconds is milliseconds here.
    const uint64_t cycles = static_cast<uint64_t>(pool) * 128;
    for (uint64_t i = 0; i < cycles; ++i)
    {
      const OrderId id = 1'000'000 + i;
      ASSERT_EQ(book.addResting(Side::BUY, resting(id, Side::BUY, 1.0, 1.0)),
                BookAddResult::Accepted)
          << "pool " << pool << ": the book refused churn order " << id;
      ASSERT_TRUE(book.cancel(id).has_value()) << "pool " << pool << ": order " << id
                                               << " was added and the index does not have it";
    }
    ASSERT_TRUE(book.empty());

    constexpr uint64_t kResting = 64;
    for (uint64_t i = 0; i < kResting; ++i)
    {
      const OrderId id = 100 + i;
      ASSERT_EQ(book.addResting(Side::BUY, resting(id, Side::BUY, 1.0 + 0.01 * double(i), 1.0)),
                BookAddResult::Accepted);
    }
    for (uint64_t i = 0; i < kResting; ++i)
    {
      const OrderId id = 100 + i;
      EXPECT_TRUE(book.contains(id)) << "pool " << pool << ": order " << id
                                     << " rests and the index lost it";
      EXPECT_NE(book.find(id), nullptr) << "pool " << pool << ": order " << id << " has no node";
      EXPECT_TRUE(book.cancel(id).has_value())
          << "pool " << pool << ": order " << id << " can never be taken off the book";
    }
    EXPECT_TRUE(book.empty()) << "pool " << pool << ": orders left on the book after cancelling all";
  }
}

// Filling the pool and emptying it again, repeatedly, at the boundary: the
// state the index is in after a full cycle is the state it started in, so the
// last node is available every time and nothing accumulates.
TEST(LadderBookIdIndex, TheFullPoolIsReusableWithoutLimit)
{
  constexpr int32_t kPool = 256;
  LadderBook book(cfgFor(kPool));

  for (int round = 0; round < 64; ++round)
  {
    for (int32_t i = 0; i < kPool; ++i)
    {
      const OrderId id = static_cast<OrderId>(round) * 1000 + static_cast<OrderId>(i) + 1;
      ASSERT_EQ(book.addResting(Side::BUY, resting(id, Side::BUY, 1.0 + 0.01 * double(i), 1.0)),
                BookAddResult::Accepted)
          << "round " << round << ": the pool did not come back";
    }
    ASSERT_TRUE(book.full()) << "round " << round << ": the pool holds " << kPool;

    // One more than the pool holds is refused, and refused for the pool rather
    // than for its price.
    EXPECT_EQ(book.addResting(Side::BUY, resting(999'999, Side::BUY, 1.0, 1.0)),
              BookAddResult::PoolExhausted);
    EXPECT_FALSE(book.contains(999'999)) << "a refused order is on no book";

    for (int32_t i = 0; i < kPool; ++i)
    {
      const OrderId id = static_cast<OrderId>(round) * 1000 + static_cast<OrderId>(i) + 1;
      ASSERT_TRUE(book.cancel(id).has_value()) << "round " << round << ": order " << id << " lost";
    }
    ASSERT_TRUE(book.empty());
  }
}

// The refusal an exhausted pool produces is not the refusal an out-of-band
// price produces, at the book's own interface: the engine maps them onto
// different reject reasons and cannot do that from one answer.
TEST(LadderBookIdIndex, TheTwoRefusalsAreDistinguishableAtTheBook)
{
  LadderBook book(LadderBook::Config{
      .basePriceRaw = px(1000.0).raw(), .tickRaw = px(10.0).raw(), .numLevels = 4, .maxOrders = 2});

  EXPECT_FALSE(book.canRest(px(995.0))) << "995 is below the base: the ladder has no level for it";
  EXPECT_EQ(book.addResting(Side::SELL, resting(1, Side::SELL, 995.0, 1.0)),
            BookAddResult::PriceOutOfBand);
  EXPECT_FALSE(book.canRest(px(1040.0))) << "the ladder is four levels: 1000, 1010, 1020, 1030";
  EXPECT_EQ(book.addResting(Side::SELL, resting(2, Side::SELL, 1040.0, 1.0)),
            BookAddResult::PriceOutOfBand);
  EXPECT_FALSE(book.full()) << "a price refusal must not consume a node";

  EXPECT_EQ(book.addResting(Side::SELL, resting(3, Side::SELL, 1000.0, 1.0)),
            BookAddResult::Accepted);
  EXPECT_EQ(book.addResting(Side::SELL, resting(4, Side::SELL, 1010.0, 1.0)),
            BookAddResult::Accepted);
  EXPECT_EQ(book.addResting(Side::SELL, resting(5, Side::SELL, 1020.0, 1.0)),
            BookAddResult::PoolExhausted);
}

// A price under the base is refused rather than filed on the base level, so a
// taker limited under the base reaches no liquidity: the truncating divide
// used to answer level 0 for both, which let a seller at 995 be shown the
// depth resting at 1000.
TEST(LadderBookIdIndex, ALimitUnderTheBaseSeesNoDepth)
{
  LadderBook book(LadderBook::Config{.basePriceRaw = px(1000.0).raw(),
                                     .tickRaw = px(10.0).raw(),
                                     .numLevels = 8,
                                     .maxOrders = 8});
  ASSERT_EQ(book.addResting(Side::SELL, resting(1, Side::SELL, 1000.0, 5.0)),
            BookAddResult::Accepted);

  EXPECT_EQ(book.availableWithin(Side::BUY, px(995.0), false).raw(), 0)
      << "a buyer limited at 995 was shown the ask resting at 1000";
  EXPECT_EQ(book.availableWithin(Side::BUY, px(1000.0), false).raw(), qty(5.0).raw());
}

// End to end, on the engine: an id the book refused is an id the engine does
// not think it has, so the same id can be sent again once there is room.
TEST(LadderBookIdIndex, ARefusedIdIsFreeToBeUsedAgain)
{
  SymbolConfig cfg;
  cfg.id = SYM;
  cfg.tickSize = px(0.01);

  std::vector<OutboundEvent> tape;
  MatchingEngine<LadderBook> eng(
      cfg, [&](const OutboundEvent& e)
      { tape.push_back(e); },
      LadderBook{LadderBook::Config{
          .basePriceRaw = 0, .tickRaw = px(0.01).raw(), .numLevels = 100'000, .maxOrders = 1}});

  const auto submit = [&](OrderId id, double price)
  {
    NewOrder o;
    o.id = id;
    o.symbol = SYM;
    o.side = Side::BUY;
    o.type = OrderType::LIMIT;
    o.price = px(price);
    o.quantity = qty(1);
    o.accountId = 7;
    eng.submit(InboundCommand{o}, static_cast<int64_t>(id));
  };

  submit(1, 10.0);
  submit(2, 11.0);  // the pool holds one

  const auto rejectFor = [&](OrderId id) -> const OrderRejected*
  {
    for (const auto& e : tape)
    {
      if (const auto* r = std::get_if<OrderRejected>(&e); r != nullptr && r->id == id)
      {
        return r;
      }
    }
    return nullptr;
  };
  ASSERT_NE(rejectFor(2), nullptr);
  EXPECT_EQ(rejectFor(2)->reason, RejectReason::BookCapacityExceeded);
  EXPECT_EQ(eng.restingOrderCount(), 1u);

  CancelOrder c;
  c.id = 1;
  c.symbol = SYM;
  c.accountId = 7;
  eng.submit(InboundCommand{c}, 3);

  // Id 2 was never on the book, so the duplicate-id gate must not remember it.
  tape.clear();
  submit(2, 11.0);
  ASSERT_EQ(rejectFor(2), nullptr) << "a refused id was held against its owner";
  EXPECT_TRUE(eng.book().contains(2));
  EXPECT_EQ(eng.restingOrderCount(), 1u);
}
