/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * engine::LastLook against a book that is a std::vector.
 *
 * The hold bookkeeping is no longer a part of the matching engine template: it
 * reaches the resting book through three operations (lift an order off its
 * level, put one back at the TAIL, read a maker as it rests) and publishes
 * through the event sink. That is a small enough seam to implement here, which
 * is the point of this file -- every rule about holds can be stated against a
 * book whose entire contents are visible as a vector, with no matcher, no
 * ledger, no journal and no session in the way.
 *
 * What the engine still answers for the component (the reference price, the
 * perp re-check, the reservation release) arrives as recorded host calls, so
 * this file also pins WHEN the component asks for them.
 *
 * The last test is not a unit test: it drives a real MatchingEngine through
 * 100k hold/resolve cycles and prints the per-cycle cost, so the seam's
 * indirection has a number attached to it rather than an opinion.
 */
#include "flox-venue/engine/last_look.h"

#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::engine::Held;
using flox::venue::engine::LastLook;

namespace
{

constexpr SymbolId SYM = 7;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

// A resting book that is a flat vector in queue order, plus a recorder for
// everything else the component asks of the engine around it.
class FakeBook
{
 public:
  std::vector<RestingOrder> resting;
  std::vector<OutboundEvent> events;
  std::vector<OutboundEvent> tracked;  // published through the last-price wrapper
  std::vector<std::string> calls;      // host calls, in the order they were made

  engine::LastLookConfig cfg{SYM, DurationNs{1000}, 0, false};
  int64_t reference{0};
  bool allowed{true};
  uint64_t tradeSeq{0};

  // ---- the seam ----

  const RestingOrder* findResting(OrderId id) const
  {
    for (const auto& o : resting)
    {
      if (o.id == id)
      {
        return &o;
      }
    }
    return nullptr;
  }

  std::optional<RestingOrder> takeResting(OrderId id)
  {
    for (auto it = resting.begin(); it != resting.end(); ++it)
    {
      if (it->id == id)
      {
        RestingOrder out = *it;
        resting.erase(it);
        return out;
      }
    }
    return std::nullopt;
  }

  void reinsertTail(Side side, const RestingOrder& o)
  {
    RestingOrder copy = o;
    copy.side = side;
    resting.push_back(copy);
  }

  void publish(const OutboundEvent& e) { events.push_back(e); }
  void publishTracked(const OutboundEvent& e)
  {
    tracked.push_back(e);
    events.push_back(e);
  }

  engine::LastLookConfig lastLookConfig() const { return cfg; }
  int64_t referenceRaw() const { return reference; }
  bool holdStillAllowed(const Held&) const { return allowed; }
  uint64_t nextTradeSeq() { return ++tradeSeq; }

  void releaseHeldLeg(OrderId id, Quantity q)
  {
    calls.push_back("release:" + std::to_string(id) + ":" + std::to_string(q.raw()));
  }
  void cleanupOrderIfDone(OrderId id)
  {
    calls.push_back("cleanup:" + std::to_string(id));
  }
  void rememberStp(OrderId id, STPMode)
  {
    calls.push_back("stp:" + std::to_string(id));
  }
  void adoptRestingTaker(const Held& h)
  {
    calls.push_back("adopt:" + std::to_string(h.taker));
  }

  // ---- what the assertions read ----

  std::vector<OrderId> queue() const
  {
    std::vector<OrderId> ids;
    for (const auto& o : resting)
    {
      ids.push_back(o.id);
    }
    return ids;
  }

  const RestingOrder* at(OrderId id) const { return findResting(id); }

  template <class T>
  int count() const
  {
    int n = 0;
    for (const auto& e : events)
    {
      if (std::get_if<T>(&e))
      {
        ++n;
      }
    }
    return n;
  }

  template <class T>
  const T* first() const
  {
    for (const auto& e : events)
    {
      if (const auto* x = std::get_if<T>(&e))
      {
        return x;
      }
    }
    return nullptr;
  }

  // Index of the first event of type T, -1 when there is none. Lets a test
  // pin the ORDER two reports were published in.
  template <class T>
  int indexOf() const
  {
    for (size_t i = 0; i < events.size(); ++i)
    {
      if (std::get_if<T>(&events[i]))
      {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
};

RestingOrder maker(OrderId id, Side s, double p, double q, uint64_t acct)
{
  RestingOrder o{id, acct, px(p), qty(q), s};
  o.lastLook = true;
  return o;
}

NewOrder taker(OrderId id, Side s, double p, double q, uint64_t acct)
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

// A taker whose residual never rests, so a test about the maker stays about
// the maker.
NewOrder ioc(NewOrder o)
{
  o.tif = TimeInForce::IOC;
  return o;
}

SeqNanos ns(int64_t v) { return SeqNanos::fromRaw(v); }

// The taker's own OrderExecuted, out of every published event -- there is
// exactly one (aggressor == true) per accepted hold, found by field rather
// than by position since the maker's report precedes it.
const OrderExecuted* takerExecuted(const FakeBook& b)
{
  for (const OutboundEvent& e : b.events)
  {
    if (const auto* x = std::get_if<OrderExecuted>(&e); x != nullptr && x->aggressor)
    {
      return x;
    }
  }
  return nullptr;
}

static_assert(engine::LastLookHost<FakeBook>,
              "the fake book must satisfy the same seam the engine does");

}  // namespace

// An accepted hold prints: one trade and one execution per leg, and nothing
// goes back on the book.
TEST(VenueEngineLastLook, AcceptPrintsTradeAndBothExecutions)
{
  FakeBook b;
  LastLook ll;
  b.resting.push_back(maker(10, Side::SELL, 100.0, 3.0, 1));

  ll.create(b, b.resting[0], qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2), ns(0));
  ASSERT_EQ(ll.openCount(), 1U);
  const FillHeld* held = b.first<FillHeld>();
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->symbol, SYM);
  EXPECT_EQ(held->makerId, 10U);
  EXPECT_EQ(held->takerId, 20U);

  ll.onDecision(b, LastLookDecision{held->heldId, SYM, true, {}, 1});
  EXPECT_EQ(ll.openCount(), 0U);
  EXPECT_EQ(b.count<Trade>(), 1);
  EXPECT_EQ(b.count<OrderExecuted>(), 2);
  EXPECT_EQ(b.count<FillRejected>(), 0);
  EXPECT_EQ(b.queue(), (std::vector<OrderId>{10}));  // untouched
  EXPECT_EQ(ll.stats().at(1).accepted, 1U);
  EXPECT_EQ(ll.stats().at(1).rejected, 0U);
}

// T062: a taker whose entire order went into one hold gets a terminal report
// when it resolves. Before the fix this leg was hardcoded leavesQty=0,
// complete=false -- truthfully zero leaves, but never marked complete, so the
// order's own terminal event never fired.
TEST(VenueEngineLastLook, TakerFullyHeldInOneHoldGetsATerminalExecution)
{
  FakeBook b;
  LastLook ll;
  b.resting.push_back(maker(10, Side::SELL, 100.0, 2.0, 1));

  // The taker's whole size (2.0) is the held quantity: nothing rests for it
  // and no sibling hold exists once this one resolves.
  ll.create(b, b.resting[0], qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2), ns(0));
  const FillHeld* held = b.first<FillHeld>();
  ASSERT_NE(held, nullptr);

  ll.onDecision(b, LastLookDecision{held->heldId, SYM, true, {}, 1});

  const OrderExecuted* tx = takerExecuted(b);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(tx->leavesQty.raw(), qty(0.0).raw());
  EXPECT_TRUE(tx->complete);
  EXPECT_EQ(tx->displayLeaves.raw(), qty(0.0).raw());
}

// T062: an IOC taker whose residual never rests (already canceled by the
// matcher before the hold could ever open) is just as terminal once its one
// hold accepts -- IOC vs GTC/GTD makes no difference here because a residual
// that never rests looks identical to one that has already been fully spent.
TEST(VenueEngineLastLook, TakerIocResidualAlreadyGoneIsStillTerminalOnAccept)
{
  FakeBook b;
  LastLook ll;
  b.resting.push_back(maker(10, Side::SELL, 100.0, 2.0, 1));

  ll.create(b, b.resting[0], qty(2.0), ioc(taker(20, Side::BUY, 100.0, 2.0, 2)), ns(0));
  const FillHeld* held = b.first<FillHeld>();
  ASSERT_NE(held, nullptr);

  ll.onDecision(b, LastLookDecision{held->heldId, SYM, true, {}, 1});

  const OrderExecuted* tx = takerExecuted(b);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(tx->leavesQty.raw(), qty(0.0).raw());
  EXPECT_TRUE(tx->complete);
}

// T062: a GTC taker that already rests on a residual (the part of its order
// that did not cross anything) is NOT done when an unrelated hold on the rest
// of its size accepts -- the resting quantity is untouched by this
// resolution, and the report has to say so honestly rather than claim zero
// leaves for an order that still has depth in the book.
TEST(VenueEngineLastLook, TakerWithARestingResidualIsNotTerminalOnAccept)
{
  FakeBook b;
  LastLook ll;
  b.resting.push_back(maker(10, Side::SELL, 100.0, 2.0, 1));
  // The taker's own residual: 1.5 left over after the hold, already resting.
  RestingOrder takerResidual{20, 2, px(100.0), qty(1.5), Side::BUY};
  b.resting.push_back(takerResidual);

  ll.create(b, b.resting[0], qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2), ns(0));
  const FillHeld* held = b.first<FillHeld>();
  ASSERT_NE(held, nullptr);

  ll.onDecision(b, LastLookDecision{held->heldId, SYM, true, {}, 1});

  const OrderExecuted* tx = takerExecuted(b);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(tx->leavesQty.raw(), qty(1.5).raw());
  EXPECT_FALSE(tx->complete);
  EXPECT_EQ(tx->displayLeaves.raw(), qty(1.5).raw());
  // The resting residual itself is untouched by this hold's resolution.
  EXPECT_EQ(b.at(20)->leaves.raw(), qty(1.5).raw());
}

// T062: one sweep can hold the same taker against more than one last-look
// maker. Accepting the first must not claim the order complete while the
// second hold -- on a different maker's clock -- is still open.
TEST(VenueEngineLastLook, TakerWithASiblingHoldStillOpenIsNotTerminalOnAccept)
{
  FakeBook b;
  LastLook ll;
  b.resting.push_back(maker(10, Side::SELL, 100.0, 2.0, 1));
  b.resting.push_back(maker(11, Side::SELL, 100.0, 1.0, 3));

  ll.create(b, b.resting[0], qty(2.0), taker(20, Side::BUY, 100.0, 3.0, 2), ns(0));
  const FillHeld* firstHeld = b.first<FillHeld>();
  ASSERT_NE(firstHeld, nullptr);
  // Read the id out now: the second create() below appends to the same
  // events vector `firstHeld` points into, and a reallocation would strand
  // the pointer.
  const uint64_t firstHeldId = firstHeld->heldId;
  ll.create(b, *b.at(11), qty(1.0), taker(20, Side::BUY, 100.0, 3.0, 2), ns(0));
  ASSERT_EQ(ll.openCount(), 2U);

  ll.onDecision(b, LastLookDecision{firstHeldId, SYM, true, {}, 1});

  const OrderExecuted* tx = takerExecuted(b);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(tx->leavesQty.raw(), qty(1.0).raw());  // the sibling hold's quantity is still owed
  EXPECT_FALSE(tx->complete);
}

// A refused hold gives the maker its quantity back, at the TAIL of its level:
// the slice loses its queue position, and the remainder that kept resting is
// merged back into the same order rather than replaced by it.
TEST(VenueEngineLastLook, RejectReturnsTheMakerAtTheTail)
{
  FakeBook b;
  LastLook ll;
  RestingOrder m = maker(10, Side::SELL, 100.0, 5.0, 1);
  b.resting.push_back(m);
  b.resting.push_back(maker(11, Side::SELL, 100.0, 3.0, 1));

  ll.create(b, m, qty(2.0), ioc(taker(20, Side::BUY, 100.0, 2.0, 2)), ns(0));
  b.resting[0].leaves = qty(3.0);  // the matcher reserved the held slice out

  ll.onDecision(b, LastLookDecision{1, SYM, false, {}, 1});

  EXPECT_EQ(b.queue(), (std::vector<OrderId>{11, 10}));  // behind the order it led
  ASSERT_NE(b.at(10), nullptr);
  EXPECT_EQ(b.at(10)->leaves.raw(), qty(5.0).raw());  // 3 resting + 2 returned
  EXPECT_EQ(b.count<FillRejected>(), 1);
  EXPECT_EQ(b.count<Trade>(), 0);
  EXPECT_EQ(ll.stats().at(1).rejected, 1U);
}

// A hold that emptied the maker's displayed size took it off the book. The
// reject has to rebuild it from the hold record -- with its own flags, not a
// bare order.
TEST(VenueEngineLastLook, RejectRebuildsAMakerHeldWhollyOutOfTheBook)
{
  FakeBook b;
  LastLook ll;
  RestingOrder m = maker(10, Side::SELL, 100.0, 2.0, 1);
  m.clientOrderId = 777;
  m.reduceOnly = true;

  ll.create(b, m, qty(2.0), ioc(taker(20, Side::BUY, 100.0, 2.0, 2)), ns(0));  // book stays empty
  ll.onDecision(b, LastLookDecision{1, SYM, false, {}, 1});

  ASSERT_EQ(b.queue(), (std::vector<OrderId>{10}));
  EXPECT_EQ(b.at(10)->side, Side::SELL);
  EXPECT_EQ(b.at(10)->leaves.raw(), qty(2.0).raw());
  EXPECT_TRUE(b.at(10)->lastLook);
  EXPECT_TRUE(b.at(10)->reduceOnly);
  EXPECT_EQ(b.at(10)->clientOrderId, 777U);
  const OrderModified* mod = b.first<OrderModified>();
  ASSERT_NE(mod, nullptr);
  EXPECT_EQ(mod->clientOrderId, 777U);
}

// The taker gets the same treatment when a hold took IT wholly off the book.
// A reduce-only leg reserves no margin (that is the point of reduce-only), so
// re-resting it as a plain order puts an order on the book that can OPEN a
// position with nothing behind it.
TEST(VenueEngineLastLook, RejectRebuildsATakerHeldWhollyOutOfTheBookWithItsFlags)
{
  FakeBook b;
  LastLook ll;
  NewOrder t = taker(20, Side::BUY, 100.0, 2.0, 2);
  t.tif = TimeInForce::GTC;  // the residual rests, so there is something to rebuild
  t.reduceOnly = true;
  t.clientOrderId = 888;

  ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), t, ns(0));
  ll.onDecision(b, LastLookDecision{1, SYM, false, {}, 1});

  ASSERT_NE(b.at(20), nullptr);
  EXPECT_EQ(b.at(20)->clientOrderId, 888U);
  EXPECT_TRUE(b.at(20)->reduceOnly) << "the restored taker can open a position with no margin";
}

// The flag restoreTaker does NOT carry, and why it does not have to. A
// post-only order that would cross is refused before the matcher reaches the
// resting side, so it never takes a last-look maker and there is no hold whose
// taker could be post-only. Pinned here so the asymmetry with reduceOnly above
// reads as a decision rather than as the same oversight twice.
TEST(VenueEngineLastLook, APostOnlyTakerNeverOpensAHold)
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.lastLookWindowNs = DurationNs{1'000'000'000};

  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> e(c, [&ev](const OutboundEvent& x)
                                 { ev.push_back(x); });
  NewOrder m = taker(10, Side::SELL, 100.0, 2.0, 1);
  m.lastLook = true;
  e.submit(InboundCommand{m}, 1);
  NewOrder t = taker(20, Side::BUY, 100.0, 2.0, 2);
  t.postOnly = true;
  ev.clear();
  e.submit(InboundCommand{t}, 2);

  EXPECT_EQ(e.openHolds(), 0U);
  const OrderRejected* rej = nullptr;
  for (const OutboundEvent& x : ev)
  {
    if (const auto* r = std::get_if<OrderRejected>(&x))
    {
      rej = r;
    }
  }
  ASSERT_NE(rej, nullptr);
  EXPECT_EQ(rej->reason, RejectReason::PostOnlyWouldCross);
}

// The maker is restored before the taker: both reports describe the same
// moment, and the order they are published in is part of the stream.
TEST(VenueEngineLastLook, RefusedLegsAreRestoredMakerFirst)
{
  FakeBook b;
  LastLook ll;
  RestingOrder m = maker(10, Side::SELL, 100.0, 2.0, 1);

  ll.create(b, m, qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2), ns(0));
  ll.onDecision(b, LastLookDecision{1, SYM, false, {}, 1});

  const int makerReport = b.indexOf<OrderModified>();
  const int takerReport = b.indexOf<OrderAccepted>();
  ASSERT_GE(makerReport, 0);
  ASSERT_GE(takerReport, 0);
  EXPECT_LT(makerReport, takerReport);
  EXPECT_EQ(b.queue(), (std::vector<OrderId>{10, 20}));
}

// The taker residual follows its TIF: a resting one comes back under its own
// id and is re-registered; one that never rests releases its buying power and
// cancels with the reason its TIF would have produced.
TEST(VenueEngineLastLook, RejectRoutesTheTakerByItsTimeInForce)
{
  {
    FakeBook b;
    LastLook ll;
    NewOrder t = taker(20, Side::BUY, 100.0, 2.0, 2);
    t.expiryNs = ns(9999);
    t.tif = TimeInForce::GTD;
    ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), t, ns(0));
    ll.onDecision(b, LastLookDecision{1, SYM, false, {}, 1});

    EXPECT_EQ(b.queue(), (std::vector<OrderId>{10, 20}));
    EXPECT_NE(std::find(b.calls.begin(), b.calls.end(), "adopt:20"), b.calls.end());
    EXPECT_EQ(b.count<OrderAccepted>(), 1);
    EXPECT_EQ(b.count<OrderCanceled>(), 0);
  }
  {
    FakeBook b;
    LastLook ll;
    NewOrder t = taker(20, Side::BUY, 100.0, 2.0, 2);
    t.tif = TimeInForce::IOC;
    ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), t, ns(0));
    ll.onDecision(b, LastLookDecision{1, SYM, false, {}, 1});

    EXPECT_EQ(b.queue(), (std::vector<OrderId>{10}));  // the taker did not rest
    EXPECT_NE(std::find(b.calls.begin(), b.calls.end(),
                        "release:20:" + std::to_string(qty(2.0).raw())),
              b.calls.end());
    const OrderCanceled* c = b.first<OrderCanceled>();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->reason, CancelReason::ImmediateOrCancelResidual);
  }
}

// A hold whose deadline has arrived expires -- ON the deadline, not one tick
// after it.
TEST(VenueEngineLastLook, TheDeadlineExpiresOnTheDot)
{
  FakeBook b;
  LastLook ll;
  b.cfg.window = DurationNs{100};
  ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2),
            ns(0));

  ll.expire(b, ns(99));
  EXPECT_EQ(ll.openCount(), 1U);  // not yet

  ll.expire(b, ns(100));
  EXPECT_EQ(ll.openCount(), 0U);
  EXPECT_EQ(b.count<FillRejected>(), 1);
  EXPECT_EQ(b.count<Trade>(), 0);
}

// ... and becomes whatever the venue said an unanswered hold becomes.
TEST(VenueEngineLastLook, TimeoutFollowsTheVenueSetting)
{
  FakeBook b;
  LastLook ll;
  b.cfg.window = DurationNs{100};
  b.cfg.acceptOnTimeout = true;
  ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2),
            ns(0));

  ll.expire(b, ns(500));
  EXPECT_EQ(b.count<Trade>(), 1);
  EXPECT_EQ(b.count<FillRejected>(), 0);
  EXPECT_EQ(ll.stats().at(1).accepted, 1U);
}

// The conduct statistic splits refusals by which way the market had moved.
// The reference is stamped after the matching pass, not at hold time.
TEST(VenueEngineLastLook, StatsSplitRefusalsByDirection)
{
  FakeBook b;
  LastLook ll;

  // A maker that sold: a rising reference is the move against it.
  b.reference = 1000;
  ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2),
            ns(0));
  ll.stampFresh(b);
  b.reference = 1200;
  ll.onDecision(b, LastLookDecision{1, SYM, false, {}, 1});

  // The same maker refusing when the move went its way.
  b.reference = 1000;
  ll.create(b, maker(11, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(21, Side::BUY, 100.0, 2.0, 2),
            ns(0));
  ll.stampFresh(b);
  b.reference = 800;
  ll.onDecision(b, LastLookDecision{2, SYM, false, {}, 1});

  const LastLookStats& st = ll.stats().at(1);
  EXPECT_EQ(st.held, 2U);
  EXPECT_EQ(st.rejected, 2U);
  EXPECT_EQ(st.accepted, 0U);
  EXPECT_EQ(st.adverse, 1U);
  EXPECT_EQ(st.rejectedAdverse, 1U);
  EXPECT_EQ(st.favourable, 1U);
  EXPECT_EQ(st.rejectedFavourable, 1U);
}

// A hold is stamped only once the matching pass is over: the stamp belongs to
// the book as it stands FOR the hold, not to the book that existed before it.
TEST(VenueEngineLastLook, WithoutAStampNoMoveIsMeasurable)
{
  FakeBook b;
  LastLook ll;
  b.reference = 1000;
  ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2),
            ns(0));
  b.reference = 5000;  // never stamped: unmeasurable, not "moved 4000"
  ll.onDecision(b, LastLookDecision{1, SYM, false, {}, 1});

  const LastLookStats& st = ll.stats().at(1);
  EXPECT_EQ(st.adverse, 0U);
  EXPECT_EQ(st.favourable, 0U);
}

// The venue refuses on magnitude alone, whatever the maker answered.
TEST(VenueEngineLastLook, VenueToleranceOverridesAnAccept)
{
  FakeBook b;
  LastLook ll;
  b.cfg.toleranceRaw = 100;
  b.reference = 1000;
  ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2),
            ns(0));
  ll.stampFresh(b);

  b.reference = 1050;  // inside tolerance
  ll.onDecision(b, LastLookDecision{1, SYM, true, {}, 1});
  EXPECT_EQ(b.count<Trade>(), 1);
  EXPECT_EQ(ll.toleranceRejected(), 0U);

  b.reference = 1000;
  ll.create(b, maker(11, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(21, Side::BUY, 100.0, 2.0, 2),
            ns(0));
  ll.stampFresh(b);
  b.reference = 1200;  // outside it, in the maker's favour -- still refused
  ll.onDecision(b, LastLookDecision{2, SYM, true, {}, 1});
  EXPECT_EQ(b.count<Trade>(), 1);
  EXPECT_EQ(b.count<FillRejected>(), 1);
  EXPECT_EQ(ll.toleranceRejected(), 1U);
}

// An accept is re-measured against the position as it is now, not as it was
// when the hold opened.
TEST(VenueEngineLastLook, ARiskLimitReachedDuringTheWindowRefusesTheAccept)
{
  FakeBook b;
  LastLook ll;
  ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2),
            ns(0));
  b.allowed = false;
  ll.onDecision(b, LastLookDecision{1, SYM, true, {}, 1});

  EXPECT_EQ(b.count<Trade>(), 0);
  EXPECT_EQ(b.count<FillRejected>(), 1);
  EXPECT_EQ(ll.riskRejected(), 1U);
}

// Only the maker whose quote is held may decide its fate, and an unknown id
// is a reject rather than a crash.
TEST(VenueEngineLastLook, OwnershipGuardsTheDecision)
{
  FakeBook b;
  LastLook ll;
  ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2),
            ns(0));

  ll.onDecision(b, LastLookDecision{999, SYM, true, {}, 1});
  ll.onDecision(b, LastLookDecision{1, SYM, true, {}, 2});  // the taker's account
  EXPECT_EQ(ll.openCount(), 1U);
  ASSERT_EQ(b.count<OrderRejected>(), 2);
  EXPECT_EQ(b.first<OrderRejected>()->reason, RejectReason::UnknownOrder);
}

// Holds are resolved in id order, never in hold-table order: the resolution
// order feeds the event stream.
TEST(VenueEngineLastLook, BulkRejectsResolveInIdOrder)
{
  FakeBook b;
  LastLook ll;
  for (int i = 0; i < 16; ++i)
  {
    const OrderId m = static_cast<OrderId>(100 + i);
    const OrderId t = static_cast<OrderId>(200 + i);
    ll.create(b, maker(m, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(t, Side::BUY, 100.0, 2.0, 2),
              ns(0));
  }
  ASSERT_EQ(ll.openCount(), 16U);

  ll.rejectAll(b);
  EXPECT_EQ(ll.openCount(), 0U);
  std::vector<uint64_t> ids;
  for (const auto& e : b.events)
  {
    if (const auto* r = std::get_if<FillRejected>(&e))
    {
      ids.push_back(r->heldId);
    }
  }
  ASSERT_EQ(ids.size(), 16U);
  EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
}

// Scoped rejects: by order id, and by account.
TEST(VenueEngineLastLook, RejectsAreScopedByOrderAndByAccount)
{
  FakeBook b;
  LastLook ll;
  ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2),
            ns(0));
  ll.create(b, maker(11, Side::SELL, 100.0, 2.0, 3), qty(2.0), taker(21, Side::BUY, 100.0, 2.0, 4),
            ns(0));

  EXPECT_TRUE(ll.referencesOrder(20));
  EXPECT_FALSE(ll.referencesOrder(99));
  EXPECT_EQ(ll.heldQtyFor(10).raw(), qty(2.0).raw());

  ll.rejectFor(b, 20);
  EXPECT_EQ(ll.openCount(), 1U);
  ll.rejectForAccount(b, 3);
  EXPECT_EQ(ll.openCount(), 0U);
}

// What the checkpoint needs: the holds in id order, the id sequence, and a
// clone that carries both and nothing else.
TEST(VenueEngineLastLook, CheckpointSurfaceRoundTrips)
{
  FakeBook b;
  LastLook ll;
  ll.create(b, maker(10, Side::SELL, 100.0, 2.0, 1), qty(2.0), taker(20, Side::BUY, 100.0, 2.0, 2),
            ns(0));
  ll.create(b, maker(11, Side::SELL, 100.0, 2.0, 1), qty(1.0), taker(21, Side::BUY, 100.0, 1.0, 2),
            ns(0));
  ll.onDecision(b, LastLookDecision{1, SYM, true, {}, 1});  // consumed, sequence keeps going

  EXPECT_EQ(ll.seq(), 2U);
  EXPECT_EQ(ll.sortedIds(), (std::vector<uint64_t>{2}));
  EXPECT_EQ(ll.at(2).maker, 11U);

  LastLook restored;
  restored.setSeq(ll.seq());
  restored.insertRestored(ll.at(2));
  EXPECT_EQ(restored.seq(), 2U);
  EXPECT_EQ(restored.openCount(), 1U);
  EXPECT_TRUE(restored.has(2));

  LastLook clone;
  clone.copyHoldsFrom(ll);
  EXPECT_EQ(clone.seq(), 2U);
  EXPECT_EQ(clone.sortedIds(), (std::vector<uint64_t>{2}));
  EXPECT_TRUE(clone.stats().empty());  // diagnostics stay with the live engine
}

// Not a unit test: the cost of one hold/resolve cycle through the real engine,
// so the seam's indirection is a number rather than an opinion. Two loops,
// because the two outcomes do different work: an accept prints and both legs
// leave, a reject puts the maker back on its level and cancels the residual.
// Both leave the book empty, so the cost measured is the hold path and not a
// book that silted up over 100k iterations.
//
// Correctness is asserted; the timing is printed, never thresholded -- a CI
// machine's clock is nobody's budget.
TEST(VenueEngineLastLook, HoldPathThroughput)
{
  constexpr int kCycles = 100000;

  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.lastLookWindowNs = DurationNs{1000000000000LL};  // no timeouts in the loop

  auto run = [&c](bool accept)
  {
    uint64_t sunk = 0;
    MatchingEngine<MatchingBook> e(c, [&sunk](const OutboundEvent&)
                                   { ++sunk; });
    int64_t ts = 0;
    uint64_t heldId = 0;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kCycles; ++i)
    {
      const OrderId mid = static_cast<OrderId>(3 * i + 1);
      NewOrder m = taker(mid, Side::SELL, 100.0, 1.0, 1);
      m.lastLook = true;
      e.submit(InboundCommand{m}, ++ts);
      e.submit(InboundCommand{ioc(taker(static_cast<OrderId>(3 * i + 2), Side::BUY, 100.0, 1.0,
                                        2))},
               ++ts);
      e.submit(InboundCommand{LastLookDecision{++heldId, SYM, accept, {}, 1}}, ++ts);
      if (!accept)
      {
        // The refused maker is back on its level: take it off again, so the
        // next cycle starts from the same empty book the first one did.
        e.submit(InboundCommand{CancelOrder{mid, SYM, {}, 1}}, ++ts);
      }
    }
    const auto t1 = std::chrono::steady_clock::now();

    EXPECT_EQ(e.openHolds(), 0U);
    EXPECT_EQ(e.restingOrderCount(), 0U);
    EXPECT_EQ(e.tradesGenerated(), accept ? static_cast<uint64_t>(kCycles) : 0U);

    const double ns =
        std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(t1 - t0).count();
    std::printf("hold path (%s): %d cycles, %.1f ns/cycle (%llu events)\n",
                accept ? "accept" : "reject", kCycles, ns / kCycles,
                static_cast<unsigned long long>(sunk));
  };

  run(true);
  run(false);
}
