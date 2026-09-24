/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// four places where the venue acts before it decides, and one
// where a report loses the name the submitter chose.
//
//  - matcher.h:320 / :644 / :651 / :667 -- every OrderCanceled the MATCHER
//    itself emits (the fill-time risk block, the three STP modes) hardcodes
//    clientOrderId to 0, although the resting order carries it. Every
//    engine-side cancel path passes the real value, so an owner reconciling
//    against the name it chose cannot join an STP or risk cancel to anything.
//
//  - engine/orders.inl:235 -- onModify rejects the order's open last-look
//    holds BEFORE it validates newQty, the tick and the band. A modify that
//    is then refused has already destroyed the holds; the order survives,
//    its holds do not.
//
//  - engine/quote_mmp.inl:112 -- applyQuote cancels both resting legs before
//    onNew applies the instrument-state gates, so a quote sent into a halted
//    or closed instrument pulls the maker's existing quotes and then
//    refuses both replacements. The replace is not atomic against a state
//    change.
//
//  - engine/dispatch.inl:251 -- SetRiskLimits, SetAdmissionProfile and
//    SetAccountRiskLimits are applied with no `symbol == cfg_.id` guard,
//    which SetBands, SetTriggerRef, SetStpGroup and SetFundingSchedule all
//    carry. A broadcast or misrouted record silently retunes another
//    instrument's risk.
//
// Every foreign-symbol command in the venue is IGNORED, silently: that is
// what the four guarded branches do, so the three unguarded ones must do it
// too. The control tests pin the own-symbol behaviour that must survive.

#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;
constexpr SymbolId OTHER_SYM = 77;  // never this engine's instrument
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE_ACCT = 999;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Volume vol(double v) { return Volume::fromDouble(v); }
Amount quoteAmt(double v) { return amountOf(Volume::fromDouble(v)); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  return c;
}

SymbolConfig lastLookCfg()
{
  SymbolConfig c = cfg();
  c.lastLookWindowNs = DurationNs{1000000};
  return c;
}

SymbolConfig perpCfg()
{
  SymbolConfig c = cfg();
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;  // 10%
  return c;
}

// Perp AND last look: the combination that installs both the fill-limit hook
// and the resting-hold hook (dispatch.inl), so a maker blocked at fill time
// can also be one the matcher has to resolve holds for first.
SymbolConfig perpLastLookCfg()
{
  SymbolConfig c = perpCfg();
  c.lastLookWindowNs = DurationNs{1000000};
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

struct Cap
{
  std::vector<OutboundEvent> ev;

  EventSink sink()
  {
    return [this](const OutboundEvent& e)
    { ev.push_back(e); };
  }

  void clear() { ev.clear(); }

  const OrderCanceled* cancel(OrderId id, CancelReason r) const
  {
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<OrderCanceled>(&e); x != nullptr && x->id == id && x->reason == r)
      {
        return x;
      }
    }
    return nullptr;
  }

  bool anyCancel(OrderId id) const
  {
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<OrderCanceled>(&e); x != nullptr && x->id == id)
      {
        return true;
      }
    }
    return false;
  }

  bool cancelRejected(OrderId id, RejectReason r) const
  {
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<CancelRejected>(&e); x != nullptr && x->id == id && x->reason == r)
      {
        return true;
      }
    }
    return false;
  }

  bool rejected(OrderId id, RejectReason r) const
  {
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<OrderRejected>(&e); x != nullptr && x->id == id && x->reason == r)
      {
        return true;
      }
    }
    return false;
  }

  template <class T>
  int count() const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (std::get_if<T>(&e) != nullptr)
      {
        ++n;
      }
    }
    return n;
  }
};

// Displayed quantity of one resting order, and its price: the book state a
// rejected command must not have moved.
struct Resting
{
  bool present{false};
  Price price{};
  Quantity leaves{};
};

Resting restingOf(const MatchingBook& b, OrderId id)
{
  const RestingOrder* ro = b.find(id);
  if (ro == nullptr)
  {
    return Resting{};
  }
  return Resting{true, ro->price, ro->leaves};
}

// ---- finding 14: the matcher's own cancels carry the clientOrderId ---------

// STP cancel-oldest. The resting leg is pulled by the matcher (matcher.h:644),
// not by any engine cancel path, so its report is the only one the owner
// gets for that order -- and it is the one that drops the name.
TEST(RejectAtomicity, AnStpCancelCarriesTheRestingClientOrderId)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  NewOrder resting = limit(1, Side::SELL, 100, 5, 7);
  resting.clientOrderId = 4242;
  eng.submit(InboundCommand{resting}, 1);
  ASSERT_TRUE(eng.book().contains(1));

  NewOrder aggressor = limit(2, Side::BUY, 100, 5, 7);  // same account
  aggressor.clientOrderId = 4243;
  aggressor.stp = STPMode::CancelOldest;
  eng.submit(InboundCommand{aggressor}, 2);

  const OrderCanceled* c = cap.cancel(1, CancelReason::SelfTradePrevention);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->account, 7u);
  EXPECT_EQ(c->clientOrderId, 4242u);
}

// STP cancel-both pulls the resting leg the same way and cancels the taker
// through the engine's own residual path, so the two reports must agree on
// whose name is whose.
TEST(RejectAtomicity, AnStpCancelBothCarriesBothClientOrderIds)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  NewOrder resting = limit(1, Side::SELL, 100, 5, 7);
  resting.clientOrderId = 5150;
  eng.submit(InboundCommand{resting}, 1);

  NewOrder aggressor = limit(2, Side::BUY, 100, 5, 7);
  aggressor.clientOrderId = 5151;
  aggressor.stp = STPMode::CancelBoth;
  eng.submit(InboundCommand{aggressor}, 2);

  const OrderCanceled* maker = cap.cancel(1, CancelReason::SelfTradePrevention);
  ASSERT_NE(maker, nullptr);
  EXPECT_EQ(maker->clientOrderId, 5150u);

  const OrderCanceled* taker = cap.cancel(2, CancelReason::SelfTradePrevention);
  ASSERT_NE(taker, nullptr);
  EXPECT_EQ(taker->clientOrderId, 5151u);  // engine-side path: correct today
}

// STP decrement, where the resting leg is no larger than the aggressor:
// matcher.h:667 pulls it whole, with the same hardcoded 0.
TEST(RejectAtomicity, AnStpDecrementCancelCarriesTheRestingClientOrderId)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  NewOrder resting = limit(1, Side::SELL, 100, 2, 7);
  resting.clientOrderId = 6060;
  eng.submit(InboundCommand{resting}, 1);

  NewOrder aggressor = limit(2, Side::BUY, 100, 5, 7);
  aggressor.clientOrderId = 6061;
  aggressor.stp = STPMode::Decrement;
  eng.submit(InboundCommand{aggressor}, 2);

  const OrderCanceled* c = cap.cancel(1, CancelReason::SelfTradePrevention);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->clientOrderId, 6060u);
}

// The fill-time perp risk block (matcher.h:320): a reduce-only order that
// rested through a position change is pulled mid-sweep, by the matcher.
TEST(RejectAtomicity, AFillTimeRiskCancelCarriesTheRestingClientOrderId)
{
  {
    // Firm maker: the matcher pulls it straight out of the sweep.
    Ledger led;
    led.deposit(1, QUOTE, quoteAmt(100000));
    led.deposit(2, QUOTE, quoteAmt(100000));
    led.deposit(3, QUOTE, quoteAmt(100000));

    Cap cap;
    MatchingEngine<MatchingBook> eng(perpCfg(), cap.sink());
    eng.setLedger(&led, VENUE_ACCT);

    eng.submit(InboundCommand{limit(1, Side::BUY, 100, 10, 1)}, 1);
    eng.submit(InboundCommand{limit(2, Side::SELL, 100, 10, 2)}, 2);
    ASSERT_EQ(eng.positionQty(1), qty(10).raw());  // acct 1 long 10

    NewOrder exit = limit(3, Side::SELL, 110, 10, 1);  // reduce-only exit, resting
    exit.reduceOnly = true;
    exit.clientOrderId = 31337;
    eng.submit(InboundCommand{exit}, 3);

    // The position shrinks to +4 behind the resting reduce-only order.
    eng.submit(InboundCommand{limit(4, Side::SELL, 100, 6, 1)}, 4);
    eng.submit(InboundCommand{limit(5, Side::BUY, 100, 6, 3)}, 5);
    ASSERT_EQ(eng.positionQty(1), qty(4).raw());

    cap.clear();
    eng.submit(InboundCommand{limit(6, Side::BUY, 110, 10, 2)}, 6);  // lift the whole exit

    const OrderCanceled* c = cap.cancel(3, CancelReason::ReduceOnlyNotReducing);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->account, 1u);
    EXPECT_EQ(c->clientOrderId, 31337u);
  }
  {
    // The same block on a maker the matcher cannot pull straight away: it has
    // an open last-look hold, so the resting-hold hook runs first and
    // RESHAPES the book underneath the sweep -- the held slice goes back onto
    // the level at the tail, as a rebuilt record. The blocked order's
    // identity, the name its owner chose included, has to be read off the
    // book BEFORE that hook. The path exists only when last look and the perp
    // fill-limit re-check are both on, so the config carries both.
    Ledger led;
    led.deposit(1, QUOTE, quoteAmt(100000));
    led.deposit(2, QUOTE, quoteAmt(100000));
    led.deposit(3, QUOTE, quoteAmt(100000));

    Cap cap;
    MatchingEngine<MatchingBook> eng(perpLastLookCfg(), cap.sink());
    eng.setLedger(&led, VENUE_ACCT);

    eng.submit(InboundCommand{limit(1, Side::BUY, 100, 10, 1)}, 1);
    eng.submit(InboundCommand{limit(2, Side::SELL, 100, 10, 2)}, 2);
    ASSERT_EQ(eng.positionQty(1), qty(10).raw());

    NewOrder exit = limit(3, Side::SELL, 110, 10, 1);  // reduce-only AND non-firm
    exit.reduceOnly = true;
    exit.lastLook = true;
    exit.clientOrderId = 31337;
    eng.submit(InboundCommand{exit}, 3);
    ASSERT_TRUE(eng.book().contains(3));

    // A partial lift is HELD, not traded: 3 of the 10 leave the book and the
    // hold stays open on order 3.
    eng.submit(InboundCommand{limit(4, Side::BUY, 110, 3, 2)}, 4);
    ASSERT_EQ(eng.openHolds(), 1u);
    ASSERT_EQ(restingOf(eng.book(), 3).leaves, qty(7));

    // The position goes flat behind the resting reduce-only order, so it can
    // no longer reduce anything.
    eng.submit(InboundCommand{limit(5, Side::SELL, 100, 10, 1)}, 5);
    eng.submit(InboundCommand{limit(6, Side::BUY, 100, 10, 3)}, 6);
    ASSERT_EQ(eng.positionQty(1), 0);
    ASSERT_EQ(eng.openHolds(), 1u);  // still open when the sweep below starts

    cap.clear();
    eng.submit(InboundCommand{limit(7, Side::BUY, 110, 7, 2)}, 7);

    // The hook fired: the hold resolved and its slice went back on the book,
    // which is what reshapes the level mid-sweep. Note the shape of the
    // guard: when the hook reshapes anything it answers true and the sweep
    // re-peeks, so the cancel below is only ever built from a record the hook
    // left alone. The capture before the call is what keeps that true if the
    // guard ever stops re-peeking.
    EXPECT_EQ(cap.count<FillRejected>(), 1);
    EXPECT_EQ(eng.openHolds(), 0u);

    const OrderCanceled* c = cap.cancel(3, CancelReason::ReduceOnlyNotReducing);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->account, 1u);
    EXPECT_EQ(c->clientOrderId, 31337u);
    EXPECT_FALSE(eng.book().contains(3));
  }
}

// Control: an engine-side cancel already carries the name, and must keep it.
TEST(RejectAtomicity, ControlAUserCancelCarriesTheClientOrderId)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  NewOrder o = limit(1, Side::SELL, 100, 5, 7);
  o.clientOrderId = 909;
  eng.submit(InboundCommand{o}, 1);
  eng.submit(InboundCommand{CancelOrder{1, SYM, {}, 7}}, 2);

  const OrderCanceled* c = cap.cancel(1, CancelReason::UserRequested);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->clientOrderId, 909u);
}

// ---- finding 21: a refused modify must leave the order and its holds alone -

// One resting last-look maker with an open hold, and a modify that cannot be
// accepted. The hold and the book position are the state under test: today
// rejectHoldsFor() runs first, so the hold is rejected, the held slice is
// restored to the book, and only then is the amend refused.
struct HeldMaker
{
  Ledger led;
  Cap cap;
  MatchingEngine<MatchingBook> eng;
  uint64_t heldId{0};

  HeldMaker() : eng(lastLookCfg(), cap.sink())
  {
    NewOrder maker = limit(1, Side::SELL, 100, 5, 1);
    maker.lastLook = true;
    maker.clientOrderId = 11;
    eng.submit(InboundCommand{maker}, 1);
    eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 2);  // 3 held out of the book
    for (const auto& e : cap.ev)
    {
      if (const auto* h = std::get_if<FillHeld>(&e))
      {
        heldId = h->heldId;
      }
    }
    cap.clear();
  }
};

TEST(RejectAtomicity, AModifyWithAnInvalidQuantityLeavesTheHoldsIntact)
{
  HeldMaker m;
  ASSERT_EQ(m.eng.openHolds(), 1u);
  ASSERT_TRUE(m.eng.hasHold(m.heldId));
  const Resting before = restingOf(m.eng.book(), 1);
  ASSERT_TRUE(before.present);
  ASSERT_EQ(before.leaves, qty(2));  // 5 minus the 3 held

  m.eng.submit(InboundCommand{ModifyOrder{1, SYM, {}, px(100), Quantity{}, 1}}, 3);

  EXPECT_TRUE(m.cap.cancelRejected(1, RejectReason::InvalidQuantity));
  EXPECT_EQ(m.eng.openHolds(), 1u);
  EXPECT_TRUE(m.eng.hasHold(m.heldId));
  EXPECT_EQ(m.cap.count<FillRejected>(), 0);

  const Resting after = restingOf(m.eng.book(), 1);
  EXPECT_TRUE(after.present);
  EXPECT_EQ(after.price, px(100));
  EXPECT_EQ(after.leaves, qty(2));
}

TEST(RejectAtomicity, AModifyOffTickLeavesTheHoldsIntact)
{
  HeldMaker m;
  ASSERT_EQ(m.eng.openHolds(), 1u);

  // 100.005 with a 0.01 tick: TickSizeViolation, checked after the holds go.
  m.eng.submit(InboundCommand{ModifyOrder{1, SYM, {}, px(100.005), qty(5), 1}}, 3);

  EXPECT_TRUE(m.cap.cancelRejected(1, RejectReason::TickSizeViolation));
  EXPECT_EQ(m.eng.openHolds(), 1u);
  EXPECT_TRUE(m.eng.hasHold(m.heldId));
  EXPECT_EQ(m.cap.count<FillRejected>(), 0);

  const Resting after = restingOf(m.eng.book(), 1);
  EXPECT_TRUE(after.present);
  EXPECT_EQ(after.price, px(100));
  EXPECT_EQ(after.leaves, qty(2));
}

TEST(RejectAtomicity, AModifyOutsideTheBandLeavesTheHoldsIntact)
{
  HeldMaker m;
  ASSERT_EQ(m.eng.openHolds(), 1u);

  // maxPrice is 1000: above the band, refused after the holds are gone.
  m.eng.submit(InboundCommand{ModifyOrder{1, SYM, {}, px(1500), qty(5), 1}}, 3);

  EXPECT_TRUE(m.cap.cancelRejected(1, RejectReason::InvalidPrice));
  EXPECT_EQ(m.eng.openHolds(), 1u);
  EXPECT_TRUE(m.eng.hasHold(m.heldId));
  EXPECT_EQ(m.cap.count<FillRejected>(), 0);

  const Resting after = restingOf(m.eng.book(), 1);
  EXPECT_TRUE(after.present);
  EXPECT_EQ(after.price, px(100));
  EXPECT_EQ(after.leaves, qty(2));
}

// The motivating case for deciding the amend before touching anything: a
// maker whose ENTIRE displayed size is held out of the book. It is absent
// from book_ until the holds resolve -- rejectHoldsFor is what puts it back
// -- yet it is live and the tracking index knows it. An amend on it must
// therefore be answered by the amend's own validation, not by "unknown
// order", and the hold must survive the refusal like any other.
TEST(RejectAtomicity, AModifyOnAFullyHeldOutMakerIsValidatedNotDeclaredUnknown)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(lastLookCfg(), cap.sink());

  NewOrder maker = limit(1, Side::SELL, 100, 5, 1);
  maker.lastLook = true;
  maker.clientOrderId = 11;
  eng.submit(InboundCommand{maker}, 1);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 5, 2)}, 2);  // holds the WHOLE size

  uint64_t heldId = 0;
  for (const auto& e : cap.ev)
  {
    if (const auto* h = std::get_if<FillHeld>(&e))
    {
      heldId = h->heldId;
    }
  }
  ASSERT_NE(heldId, 0u);
  ASSERT_EQ(eng.openHolds(), 1u);
  // Held out: absent from the book, and from the account snapshot too --
  // that snapshot reads the book. Only the tracking index still knows the
  // order is live, which is exactly why existence cannot come from book_.
  ASSERT_EQ(eng.book().find(1), nullptr);
  ASSERT_TRUE(eng.snapshotAccount(1).openOrders.empty());

  cap.clear();
  eng.submit(InboundCommand{ModifyOrder{1, SYM, {}, px(100), Quantity{}, 1}}, 3);

  EXPECT_TRUE(cap.cancelRejected(1, RejectReason::InvalidQuantity));
  EXPECT_FALSE(cap.cancelRejected(1, RejectReason::UnknownOrder));
  EXPECT_EQ(eng.openHolds(), 1u);
  EXPECT_TRUE(eng.hasHold(heldId));
  EXPECT_EQ(cap.count<FillRejected>(), 0);
  EXPECT_EQ(eng.book().find(1), nullptr);  // still held out, not restored
}

// A ModifyOrder naming an id the engine has never seen. The answer is the
// amend's LAST word, not its first: existence has to be settled before the
// amend's own fields are read, or an unknown id comes back with a verdict on
// a quantity or a price that belongs to no order. And nothing else may move
// -- no report but the refusal, no resting order touched, no hold resolved.
TEST(RejectAtomicity, AModifyForAnUnknownOrderIsRefusedAndTouchesNothing)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(lastLookCfg(), cap.sink());

  NewOrder maker = limit(1, Side::SELL, 100, 5, 1);
  maker.lastLook = true;
  maker.clientOrderId = 11;
  eng.submit(InboundCommand{maker}, 1);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 2);  // 3 held out
  eng.submit(InboundCommand{limit(3, Side::BUY, 95, 4, 1)}, 3);   // a second resting order

  uint64_t heldId = 0;
  for (const auto& e : cap.ev)
  {
    if (const auto* h = std::get_if<FillHeld>(&e))
    {
      heldId = h->heldId;
    }
  }
  ASSERT_NE(heldId, 0u);
  ASSERT_EQ(eng.openHolds(), 1u);
  ASSERT_EQ(restingOf(eng.book(), 1).leaves, qty(2));
  ASSERT_EQ(restingOf(eng.book(), 3).leaves, qty(4));

  // An id the venue never issued, amended three ways: a quantity that is not
  // a quantity, a price off the tick, and a perfectly ordinary amend. All
  // three answer the same thing, because the order does not exist.
  cap.clear();
  eng.submit(InboundCommand{ModifyOrder{999, SYM, {}, Price{}, Quantity{}, 1}}, 4);
  EXPECT_EQ(cap.ev.size(), 1u);
  EXPECT_TRUE(cap.cancelRejected(999, RejectReason::UnknownOrder));
  EXPECT_FALSE(cap.cancelRejected(999, RejectReason::InvalidQuantity));

  cap.clear();
  eng.submit(InboundCommand{ModifyOrder{999, SYM, {}, px(100.005), qty(1), 1}}, 5);
  EXPECT_EQ(cap.ev.size(), 1u);
  EXPECT_TRUE(cap.cancelRejected(999, RejectReason::UnknownOrder));
  EXPECT_FALSE(cap.cancelRejected(999, RejectReason::TickSizeViolation));

  cap.clear();
  eng.submit(InboundCommand{ModifyOrder{999, SYM, {}, px(99), qty(1), 1}}, 6);
  EXPECT_EQ(cap.ev.size(), 1u);
  EXPECT_TRUE(cap.cancelRejected(999, RejectReason::UnknownOrder));

  // Nothing the engine was holding moved.
  EXPECT_EQ(eng.openHolds(), 1u);
  EXPECT_TRUE(eng.hasHold(heldId));
  EXPECT_EQ(restingOf(eng.book(), 1).leaves, qty(2));
  EXPECT_EQ(restingOf(eng.book(), 1).price, px(100));
  EXPECT_EQ(restingOf(eng.book(), 3).leaves, qty(4));
  EXPECT_EQ(restingOf(eng.book(), 3).price, px(95));
  EXPECT_FALSE(eng.book().contains(999));
}

// The same question for an id that DID exist and was cancelled: the tracking
// index forgot it, so it is as unknown as one never issued, and an amend on
// it gets the same word back rather than a verdict on its fields.
TEST(RejectAtomicity, AModifyForACancelledOrderIsRefusedAsUnknown)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  NewOrder o = limit(7, Side::SELL, 105, 2, 1);
  o.clientOrderId = 77;
  eng.submit(InboundCommand{o}, 1);
  ASSERT_TRUE(eng.book().contains(7));
  eng.submit(InboundCommand{CancelOrder{7, SYM, {}, 1}}, 2);
  ASSERT_FALSE(eng.book().contains(7));

  cap.clear();
  eng.submit(InboundCommand{ModifyOrder{7, SYM, {}, Price{}, Quantity{}, 1}}, 3);
  EXPECT_EQ(cap.ev.size(), 1u);
  EXPECT_TRUE(cap.cancelRejected(7, RejectReason::UnknownOrder));
  EXPECT_FALSE(cap.cancelRejected(7, RejectReason::InvalidQuantity));

  cap.clear();
  eng.submit(InboundCommand{ModifyOrder{7, SYM, {}, px(105.005), qty(1), 1}}, 4);
  EXPECT_EQ(cap.ev.size(), 1u);
  EXPECT_TRUE(cap.cancelRejected(7, RejectReason::UnknownOrder));
  EXPECT_FALSE(cap.cancelRejected(7, RejectReason::TickSizeViolation));

  EXPECT_FALSE(eng.book().contains(7));
  EXPECT_TRUE(eng.snapshotAccount(1).openOrders.empty());
}

// A bare NewOrder -- no quote involved -- must be refused on the instrument's
// own state with the reason that state carries. applyQuote reads the same
// gate, and the quote tests above would stay green if validate() stopped
// reading it, so the order path is pinned on its own.
TEST(RejectAtomicity, ABareNewOrderIsRefusedOnTheInstrumentState)
{
  {
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
    eng.setHalted(true);
    eng.submit(InboundCommand{limit(1, Side::SELL, 100, 1, 1)}, 1);
    EXPECT_TRUE(cap.rejected(1, RejectReason::Halted));
    EXPECT_FALSE(eng.book().contains(1));
    EXPECT_TRUE(eng.snapshotAccount(1).openOrders.empty());
  }
  {
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
    eng.closeSession();
    eng.submit(InboundCommand{limit(1, Side::SELL, 100, 1, 1)}, 1);
    EXPECT_TRUE(cap.rejected(1, RejectReason::MarketClosed));
    EXPECT_FALSE(eng.book().contains(1));
    EXPECT_TRUE(eng.snapshotAccount(1).openOrders.empty());
  }
  {
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
    eng.delist();
    eng.submit(InboundCommand{limit(1, Side::SELL, 100, 1, 1)}, 1);
    EXPECT_TRUE(cap.rejected(1, RejectReason::InstrumentDelisted));
    EXPECT_FALSE(eng.book().contains(1));
    EXPECT_TRUE(eng.snapshotAccount(1).openOrders.empty());
  }
}

// Control: an ACCEPTED modify still resolves the order's holds first -- the
// order is reshaped and a hold left behind would settle against a
// reservation that no longer covers it.
TEST(RejectAtomicity, ControlAnAcceptedModifyStillRejectsTheHolds)
{
  HeldMaker m;
  ASSERT_EQ(m.eng.openHolds(), 1u);

  m.eng.submit(InboundCommand{ModifyOrder{1, SYM, {}, px(101), qty(5), 1}}, 3);

  EXPECT_EQ(m.cap.count<FillRejected>(), 1);
  EXPECT_EQ(m.eng.openHolds(), 0u);
  EXPECT_FALSE(m.eng.hasHold(m.heldId));

  const Resting after = restingOf(m.eng.book(), 1);
  EXPECT_TRUE(after.present);
  EXPECT_EQ(after.price, px(101));
  EXPECT_EQ(after.leaves, qty(5));
}

// Control: an amend refused BEFORE rejectHoldsFor (a stranger's) already
// leaves the holds alone, and must keep doing so.
TEST(RejectAtomicity, ControlAStrangersModifyLeavesTheHoldsIntact)
{
  HeldMaker m;
  ASSERT_EQ(m.eng.openHolds(), 1u);

  m.eng.submit(InboundCommand{ModifyOrder{1, SYM, {}, px(101), qty(5), 42}}, 3);

  EXPECT_TRUE(m.cap.cancelRejected(1, RejectReason::NotOrderOwner));
  EXPECT_EQ(m.eng.openHolds(), 1u);
  EXPECT_EQ(m.cap.count<FillRejected>(), 0);
}

// ---- finding 26: a quote refused on instrument state pulls nothing ---------

// The two legs of a resting quote, and a replacement sent into a halted
// instrument. The replacement cannot be accepted, so the quote the maker has
// on the book must still be there: applyQuote cancels before onNew gates.
struct QuotingMaker
{
  Cap cap;
  MatchingEngine<MatchingBook> eng;

  QuotingMaker() : eng(cfg(), cap.sink())
  {
    Quote q;
    q.bidId = 10;
    q.askId = 11;
    q.symbol = SYM;
    q.bidPrice = px(99);
    q.bidQty = qty(1);
    q.askPrice = px(101);
    q.askQty = qty(1);
    q.accountId = 1;
    q.clientOrderId = 500;
    eng.submit(InboundCommand{q}, 1);
    cap.clear();
  }

  Quote replacement(uint64_t clOrdId) const
  {
    Quote q;
    q.bidId = 10;
    q.askId = 11;
    q.symbol = SYM;
    q.bidPrice = px(98);
    q.bidQty = qty(1);
    q.askPrice = px(102);
    q.askQty = qty(1);
    q.accountId = 1;
    q.clientOrderId = clOrdId;
    return q;
  }
};

TEST(RejectAtomicity, AQuoteIntoAHaltedInstrumentLeavesBothLegsResting)
{
  QuotingMaker m;
  ASSERT_TRUE(m.eng.book().contains(10));
  ASSERT_TRUE(m.eng.book().contains(11));

  m.eng.setHalted(true);
  m.cap.clear();
  m.eng.submit(InboundCommand{m.replacement(501)}, 2);

  EXPECT_FALSE(m.cap.anyCancel(10));
  EXPECT_FALSE(m.cap.anyCancel(11));
  EXPECT_TRUE(m.eng.book().contains(10));
  EXPECT_TRUE(m.eng.book().contains(11));

  const Resting bid = restingOf(m.eng.book(), 10);
  const Resting ask = restingOf(m.eng.book(), 11);
  ASSERT_TRUE(bid.present);
  ASSERT_TRUE(ask.present);
  EXPECT_EQ(bid.price, px(99));  // the OLD quote, untouched
  EXPECT_EQ(ask.price, px(101));
  EXPECT_EQ(m.eng.snapshotAccount(1).openOrders.size(), 2u);
  EXPECT_TRUE(m.cap.rejected(10, RejectReason::Halted));

  // The refusal is a refusal, not a non-event: the quote was received, so it
  // SPENT its clientOrderId. The dedup index is registered before the state
  // is read, which is the order a plain order gets too -- a resend of a name
  // the venue has already seen is refused whatever happened to the first
  // submission. Replaying the same name once the instrument trades again is
  // therefore a duplicate, and the quote on the book is left alone.
  m.eng.setHalted(false);
  m.cap.clear();
  m.eng.submit(InboundCommand{m.replacement(501)}, 3);

  EXPECT_TRUE(m.cap.rejected(10, RejectReason::DuplicateClientOrderId));
  EXPECT_FALSE(m.cap.anyCancel(10));
  EXPECT_FALSE(m.cap.anyCancel(11));
  const Resting bidAfter = restingOf(m.eng.book(), 10);
  const Resting askAfter = restingOf(m.eng.book(), 11);
  ASSERT_TRUE(bidAfter.present);
  ASSERT_TRUE(askAfter.present);
  EXPECT_EQ(bidAfter.price, px(99));  // still the ORIGINAL quote
  EXPECT_EQ(askAfter.price, px(101));

  // A name the venue has NOT seen still works, so the account is not stuck.
  m.cap.clear();
  m.eng.submit(InboundCommand{m.replacement(599)}, 4);
  EXPECT_EQ(restingOf(m.eng.book(), 10).price, px(98));
  EXPECT_EQ(restingOf(m.eng.book(), 11).price, px(102));
}

TEST(RejectAtomicity, AQuoteIntoAClosedInstrumentLeavesBothLegsResting)
{
  QuotingMaker m;
  ASSERT_TRUE(m.eng.book().contains(10));

  m.eng.closeSession();  // outside the session: onNew refuses both legs
  m.cap.clear();
  m.eng.submit(InboundCommand{m.replacement(502)}, 2);

  EXPECT_FALSE(m.cap.anyCancel(10));
  EXPECT_FALSE(m.cap.anyCancel(11));
  EXPECT_TRUE(m.eng.book().contains(10));
  EXPECT_TRUE(m.eng.book().contains(11));
  EXPECT_EQ(m.eng.snapshotAccount(1).openOrders.size(), 2u);
}

// Control: on a trading instrument the quote still replaces both legs.
TEST(RejectAtomicity, ControlAQuoteOnATradingInstrumentStillReplacesBothLegs)
{
  QuotingMaker m;
  m.eng.submit(InboundCommand{m.replacement(503)}, 2);

  EXPECT_TRUE(m.cap.anyCancel(10));
  EXPECT_TRUE(m.cap.anyCancel(11));

  const Resting bid = restingOf(m.eng.book(), 10);
  const Resting ask = restingOf(m.eng.book(), 11);
  ASSERT_TRUE(bid.present);
  ASSERT_TRUE(ask.present);
  EXPECT_EQ(bid.price, px(98));
  EXPECT_EQ(ask.price, px(102));
}

// ---- finding 22: a foreign symbol must not retune this engine's risk ------

SetRiskLimits fatFingerLimits(SymbolId sym, double maxQty)
{
  SetRiskLimits r;
  r.symbol = sym;
  r.fields = RiskLimitField::RiskFatFinger;
  r.maxOrderQty = qty(maxQty);
  r.maxOrderNotional = vol(0);
  return r;
}

TEST(RejectAtomicity, SetRiskLimitsForAForeignSymbolIsIgnored)
{
  Cap cap;
  SymbolConfig c = cfg();
  c.maxOrderQty = qty(10);
  MatchingEngine<MatchingBook> eng(c, cap.sink());

  eng.submit(InboundCommand{fatFingerLimits(OTHER_SYM, 1)}, 1);

  // Ignored the way every other foreign-symbol record is: no reply, no
  // report, nothing on the stream at all. A guard that refused with an event
  // would be a different contract from SetBands and the rest.
  EXPECT_TRUE(cap.ev.empty());
  EXPECT_EQ(eng.config().maxOrderQty, qty(10));
  EXPECT_EQ(eng.riskLimits().maxOrderQty, qty(10));

  // And the gate still behaves as it was configured: 5 is under this
  // instrument's cap, and only the foreign record said otherwise.
  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5, 1)}, 2);
  EXPECT_FALSE(cap.rejected(1, RejectReason::OrderTooLarge));
  EXPECT_TRUE(eng.book().contains(1));
}

TEST(RejectAtomicity, SetAdmissionProfileForAForeignSymbolIsIgnored)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  SetAdmissionProfile p;
  p.symbol = OTHER_SYM;
  p.account = 1;
  p.profile.deny = AdmissionDeny::DenyNewOrder;
  eng.submit(InboundCommand{p}, 1);

  EXPECT_TRUE(cap.ev.empty());  // silently ignored, not refused with an event
  EXPECT_EQ(eng.admissionProfiles().count(1), 0u);

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 1, 1)}, 2);
  EXPECT_FALSE(cap.rejected(1, RejectReason::NewOrderNotPermitted));
  EXPECT_TRUE(eng.book().contains(1));
}

TEST(RejectAtomicity, SetAccountRiskLimitsForAForeignSymbolIsIgnored)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  SetAccountRiskLimits a;
  a.symbol = OTHER_SYM;
  a.account = 1;
  a.fields = AccountRiskLimitField::AccountRiskFatFinger;
  a.maxOrderQty = qty(1);
  a.maxOrderNotional = vol(0);
  eng.submit(InboundCommand{a}, 1);

  EXPECT_TRUE(cap.ev.empty());  // silently ignored, not refused with an event
  EXPECT_EQ(eng.accountRiskLimits(1), nullptr);

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5, 1)}, 2);
  EXPECT_FALSE(cap.rejected(1, RejectReason::OrderTooLarge));
  EXPECT_TRUE(eng.book().contains(1));
}

// Control: the same three records on the engine's OWN symbol still apply.
TEST(RejectAtomicity, ControlTheThreeLimitCommandsStillApplyOnTheOwnSymbol)
{
  Cap cap;
  SymbolConfig c = cfg();
  c.maxOrderQty = qty(10);
  MatchingEngine<MatchingBook> eng(c, cap.sink());

  eng.submit(InboundCommand{fatFingerLimits(SYM, 1)}, 1);
  EXPECT_EQ(eng.config().maxOrderQty, qty(1));
  EXPECT_EQ(eng.riskLimits().maxOrderQty, qty(1));

  SetAccountRiskLimits a;
  a.symbol = SYM;
  a.account = 2;
  a.fields = AccountRiskLimitField::AccountRiskMaxOpenOrders;
  a.maxOpenOrders = 3;
  eng.submit(InboundCommand{a}, 2);
  ASSERT_NE(eng.accountRiskLimits(2), nullptr);
  EXPECT_EQ(eng.accountRiskLimits(2)->maxOpenOrders, 3u);

  SetAdmissionProfile p;
  p.symbol = SYM;
  p.account = 3;
  p.profile.deny = AdmissionDeny::DenyNewOrder;
  eng.submit(InboundCommand{p}, 3);
  EXPECT_EQ(eng.admissionProfiles().count(3), 1u);

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5, 1)}, 4);
  EXPECT_TRUE(cap.rejected(1, RejectReason::OrderTooLarge));  // the symbol's new cap binds
  eng.submit(InboundCommand{limit(2, Side::SELL, 100, 1, 3)}, 5);
  EXPECT_TRUE(cap.rejected(2, RejectReason::NewOrderNotPermitted));  // the profile binds
}

}  // namespace
