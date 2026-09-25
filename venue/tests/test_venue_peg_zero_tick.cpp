/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Pegged orders on an instrument that declares no tick.
 *
 * `tickSize == 0` means "unchecked" everywhere else in SymbolConfig, but
 * PegBook::targetRaw reads it as a DISTANCE: a buy peg that would cross is
 * pulled back to `askRaw - tickRaw`, which with no tick is the opposite touch
 * itself. repeg() then re-rests the order through addResting, and that path
 * runs no matching pass -- so the order comes back onto the book AT the ask
 * and the instrument quotes bid == ask, a locked book nobody can trade out of.
 *
 * The invariant these tests pin is the one the clamp exists for: a pegged
 * order never rests at or through the price it is tracking. Either the venue
 * refuses the peg (at configuration or at admission) with a reason the owner
 * can act on, or the peg rests strictly inside the touch -- it must not
 * quietly lock the book instead.
 */
#include "flox-venue/engine/pegs.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

// `tick` is the only difference between the two instruments these tests use:
// 0.01 is an ordinary venue, 0 is the one whose peg clamp has no distance to
// work with.
SymbolConfig cfg(double tick)
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(tick);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}

struct Venue
{
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng;

  explicit Venue(const SymbolConfig& c) : eng(c, [this](const OutboundEvent& e)
                                              { ev.push_back(e); })
  {
  }

  const OrderRejected* rejectOf(OrderId id) const
  {
    for (const auto& e : ev)
    {
      if (const auto* r = std::get_if<OrderRejected>(&e); r != nullptr && r->id == id)
      {
        return r;
      }
    }
    return nullptr;
  }

  bool stillResting(OrderId id) const
  {
    for (const auto& e : ev)
    {
      if (const auto* c = std::get_if<OrderCanceled>(&e); c != nullptr && c->id == id)
      {
        return false;
      }
    }
    return rejectOf(id) == nullptr;
  }
};

// An ask-pegged buy that starts with no ask to track, then an ask arrives
// above it: the arrival is a submit boundary, so repeg() re-prices the peg
// onto the new touch. This is the sequence that reaches the clamp through
// addResting rather than through the matcher.
//
// Order 1 sets a bid the peg can fall back to; order 2 is the peg; order 3 is
// the ask that shows up afterwards, priced clear of the bid so it rests
// instead of trading.
void arrangeAskThenRepeg(Venue& v)
{
  v.eng.submit(InboundCommand{limit(1, Side::BUY, 99.0, 5.0, 1)}, 1);
  NewOrder peg = limit(2, Side::BUY, 0.0, 4.0, 2);
  peg.peg = PegRef::Ask;
  peg.pegOffsetRaw = 0;
  v.eng.submit(InboundCommand{peg}, 2);
  v.eng.submit(InboundCommand{limit(3, Side::SELL, 105.0, 5.0, 3)}, 3);
}

// The mirror of the sequence above on the other side: a bid-pegged sell with
// a bid to track. Nothing about the clamp is side-specific -- a sell pulled
// onto the bid locks the book exactly the way a buy pulled onto the ask does
// -- so the refusal must not be either.
void arrangeBidThenRepeg(Venue& v)
{
  v.eng.submit(InboundCommand{limit(1, Side::SELL, 106.0, 5.0, 1)}, 1);
  NewOrder peg = limit(2, Side::SELL, 0.0, 4.0, 2);
  peg.peg = PegRef::Bid;
  peg.pegOffsetRaw = 0;
  v.eng.submit(InboundCommand{peg}, 2);
  v.eng.submit(InboundCommand{limit(3, Side::BUY, 100.0, 5.0, 3)}, 3);
}

}  // namespace

// Control: with a tick the clamp has a distance, the repriced peg stops one
// tick inside the ask, and the book still has a spread. Green today -- it is
// what says the sequence below reaches the peg path at all.
TEST(VenuePegZeroTick, TickedPegStopsOneTickInsideTheTouch)
{
  Venue v(cfg(0.01));
  arrangeAskThenRepeg(v);

  ASSERT_TRUE(v.eng.book().bestBid().has_value());
  ASSERT_TRUE(v.eng.book().bestAsk().has_value());
  EXPECT_EQ(v.eng.book().bestBid().value(), px(104.99));
  EXPECT_EQ(v.eng.book().bestAsk().value(), px(105.00));
  EXPECT_LT(v.eng.book().bestBid().value(), v.eng.book().bestAsk().value());
}

// The same sequence on an instrument with no tick must not produce a locked
// book. Whichever way the venue chooses to refuse the peg, the resting state
// afterwards has to keep bid < ask.
TEST(VenuePegZeroTick, ZeroTickPegNeverLocksTheBook)
{
  Venue v(cfg(0.0));
  arrangeAskThenRepeg(v);

  ASSERT_TRUE(v.eng.book().bestAsk().has_value());
  const Price ask = v.eng.book().bestAsk().value();
  if (v.eng.book().bestBid().has_value())
  {
    EXPECT_LT(v.eng.book().bestBid().value(), ask)
        << "a zero-tick peg was re-rested at the opposite touch: the book is locked";
  }
}

// ... and the peg itself must never come to rest at or through the price it
// tracks. Stated on the order rather than on the touch so a fix that leaves
// some other order at the best bid cannot satisfy the test by accident.
TEST(VenuePegZeroTick, ZeroTickPegNeverRestsThroughItsReference)
{
  Venue v(cfg(0.0));
  arrangeAskThenRepeg(v);

  ASSERT_TRUE(v.eng.book().bestAsk().has_value());
  const Price ask = v.eng.book().bestAsk().value();
  const auto snap = v.eng.snapshotAccount(2);
  for (const auto& o : snap.openOrders)
  {
    if (o.id == 2)
    {
      EXPECT_LT(o.price, ask) << "the peg rests at or through the reference it tracks";
    }
  }
}

// The refusal the finding asks for: a peg the venue cannot clamp is turned
// down with a reason, not accepted and then clamped onto the touch. Either
// the instrument refuses to carry pegs at all (configuration) or the order is
// rejected (admission); both surface as a reject the owner can read, and a
// peg that survives must at least not be resting on the reference.
TEST(VenuePegZeroTick, ZeroTickPegIsRefusedWithAReason)
{
  Venue v(cfg(0.0));
  arrangeAskThenRepeg(v);

  const OrderRejected* r = v.rejectOf(2);
  const bool refused = r != nullptr && r->reason != RejectReason::None;
  const bool pulled = !v.stillResting(2);
  EXPECT_TRUE(refused || pulled)
      << "a peg on a tick-less instrument was accepted and left resting: the venue has no "
         "distance to clamp it with, so it has to say no rather than lock the book";
  if (r != nullptr)
  {
    EXPECT_NE(r->reason, RejectReason::None) << "a refusal with no reason tells the owner nothing";
  }
}

// Which reason, not just that there is one. TickSizeViolation says "your
// price is off the grid" and sends the owner back with a rounded price, and
// no rounding can help here: what is missing belongs to the instrument, not
// to the order. The owner has to be told to give the instrument a tick, so
// the refusal has to name the thing that is missing.
TEST(VenuePegZeroTick, ZeroTickPegIsRefusedWithPegRequiresTick)
{
  Venue v(cfg(0.0));
  arrangeAskThenRepeg(v);

  const OrderRejected* r = v.rejectOf(2);
  ASSERT_NE(r, nullptr) << "the peg was not refused at all";
  EXPECT_EQ(r->reason, RejectReason::PegRequiresTick)
      << "a reason that tells the owner to round a price cannot be acted on: the instrument, "
         "not the order, is what has no tick";
}

// The instrument has no tick on either side of the book. A refusal that reads
// the order's side refuses half the pegs that lock the book and lets the
// other half through.
TEST(VenuePegZeroTick, ZeroTickSellPegIsRefusedTheSameWay)
{
  Venue v(cfg(0.0));
  arrangeBidThenRepeg(v);

  const OrderRejected* r = v.rejectOf(2);
  ASSERT_NE(r, nullptr) << "a sell peg on a tick-less instrument was admitted";
  EXPECT_EQ(r->reason, RejectReason::PegRequiresTick);

  // And it is not resting: the book that a bid-pegged sell would have been
  // clamped onto still has a spread.
  ASSERT_TRUE(v.eng.book().bestBid().has_value());
  if (v.eng.book().bestAsk().has_value())
  {
    EXPECT_LT(v.eng.book().bestBid().value(), v.eng.book().bestAsk().value())
        << "a zero-tick sell peg was re-rested at the opposite touch: the book is locked";
  }
}

// ---- the clamp itself ---------------------------------------------------
//
// PegBook::targetRaw is a pure decision over a Market struct, which is the
// second line of defence behind the admission refusal above: a peg restored
// from a snapshot an older build wrote never passes through validate(), so
// the clamp has to hold on its own. Asserted on the function directly for
// that reason -- through the engine these branches are unreachable, and an
// unreachable defence is one nobody would notice breaking.

// With no tick declared there is still a smallest step that leaves the peg
// strictly inside the touch: one raw unit. Stepping back by the tick itself
// -- zero -- puts the order ON the opposite touch, and repeg() re-rests it
// there through a path that runs no matching pass.
TEST(VenuePegZeroTick, ClampStepsBackOneRawUnitWhenThereIsNoTick)
{
  PegBook::Market m;
  m.hasAsk = true;
  m.askRaw = 10'500'000'000;
  m.tickRaw = 0;

  const int64_t buy = PegBook::targetRaw(Side::BUY, PegRef::Ask, 0, m);
  EXPECT_EQ(buy, m.askRaw - 1) << "a buy peg with no tick was clamped onto the ask itself";
  EXPECT_LT(buy, m.askRaw);

  PegBook::Market n;
  n.hasBid = true;
  n.bidRaw = 9'900'000'000;
  n.tickRaw = 0;

  const int64_t sell = PegBook::targetRaw(Side::SELL, PegRef::Bid, 0, n);
  EXPECT_EQ(sell, n.bidRaw + 1) << "a sell peg with no tick was clamped onto the bid itself";
  EXPECT_GT(sell, n.bidRaw);
}

// Tick alignment goes DOWN, which for a negative target is not what integer
// division does: truncation rounds toward zero, so a target below zero would
// be aligned UP -- the one direction the never-cross clamp must never be
// handed a value from. A negative target is reachable through a large
// negative offset, which no admission check refuses.
TEST(VenuePegZeroTick, ClampAlignsDownForATargetBelowZero)
{
  PegBook::Market m;
  m.hasBid = true;
  m.bidRaw = 0;
  m.tickRaw = 10;

  // Reference 0, offset -25: the aligned-down target is -30, not -20.
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Bid, -25, m), -30)
      << "a negative target was aligned up: truncating division rounds toward zero";

  // Exactly on a tick is left alone, whichever side of zero it is on.
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Bid, -30, m), -30);
  EXPECT_EQ(PegBook::targetRaw(Side::BUY, PegRef::Bid, 25, m), 20);
}
