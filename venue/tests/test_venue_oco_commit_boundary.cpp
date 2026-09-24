/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// where onNew declares an OCO leg committed, against where it can
// still refuse it.
//
// engine/oco.h states the rule the OcoCleanup guard in validate.inl exists to
// keep: a leg that leaves the venue by any door but stays in members_ will
// later cancel whatever order REUSES its id. The guard is armed by
// `committed`, and `committed` is set at validate.inl:456 -- while three
// refusals still lie ahead of it: the LULD band, the matcher's own
// out.reject (post-only would cross, FOK unfulfillable) and the zero-fill
// residual cancel. A leg refused by any of those is dead, unlinked from
// nothing, and its id stays in the group.
//
// Each test below refuses one leg past that line, has the SAME ACCOUNT reuse
// the dead id for an unrelated order, and then fills the surviving sibling.
// The reused order must be untouched: no OcoTriggered for that id, and the
// order still resting where it was put. The three control tests at the end
// pin what must NOT change -- a real sibling still loses, a leg refused
// BEFORE the commit point is still unlinked, and a conditional parked in the
// stop book still counts as committed.

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

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  return c;
}

SymbolConfig luldCfg()
{
  SymbolConfig c = cfg();
  c.luldBps = 100;                  // +/- 1%
  c.luldHaltNs = DurationNs{1000};  // pause length
  return c;
}

SymbolConfig lastLookCfg()
{
  SymbolConfig c = cfg();
  c.lastLookWindowNs = DurationNs{1000000};
  return c;
}

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct,
               TimeInForce tif = TimeInForce::GTC)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.tif = tif;
  o.accountId = acct;
  return o;
}

NewOrder stopMarket(OrderId id, Side s, double q, double trigger, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::STOP_MARKET;
  o.quantity = qty(q);
  o.triggerPrice = px(trigger);
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

  bool canceled(OrderId id, CancelReason r) const
  {
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<OrderCanceled>(&e); x != nullptr && x->id == id && x->reason == r)
      {
        return true;
      }
    }
    return false;
  }

  int trades() const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (std::get_if<Trade>(&e) != nullptr)
      {
        ++n;
      }
    }
    return n;
  }

  int cancelCount(CancelReason r) const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<OrderCanceled>(&e); x != nullptr && x->reason == r)
      {
        ++n;
      }
    }
    return n;
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

// The reused id as the account sees it: resting, at the price the reuse
// asked for. `hasOpenOrder` is the reconciliation question -- the owner's
// order list is what a counterparty reads back.
bool hasOpenOrder(const MatchingEngine<MatchingBook>& eng, uint64_t acct, OrderId id, double price)
{
  for (const auto& v : eng.snapshotAccount(acct).openOrders)
  {
    if (v.id == id)
    {
      return v.price == px(price);
    }
  }
  return false;
}

// ---- the three refusals past the commit point -----------------------------

// Post-only would cross: matcher_.cross() returns out.reject AFTER
// validate.inl set committed = true, so the guard does not unlink id 11 and
// group 5 keeps a dead member.
TEST(OcoCommitBoundary, APostOnlyRefusalLeavesNoDeadIdInTheGroup)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{limit(1, Side::SELL, 105, 1, 2)}, 1);  // crossing liquidity, acct 2

  NewOrder legA = limit(10, Side::SELL, 110, 1, 1);
  legA.ocoGroup = 5;
  eng.submit(InboundCommand{legA}, 2);  // group 5 = [10], resting

  NewOrder legB = limit(11, Side::BUY, 105, 1, 1);
  legB.ocoGroup = 5;
  legB.postOnly = true;
  eng.submit(InboundCommand{legB}, 3);  // refused at the matcher: past the commit point
  ASSERT_TRUE(cap.rejected(11, RejectReason::PostOnlyWouldCross));
  EXPECT_FALSE(eng.book().contains(11));

  // The same account reuses the dead id for an unrelated order.
  eng.submit(InboundCommand{limit(11, Side::BUY, 95, 1, 1)}, 4);
  ASSERT_TRUE(hasOpenOrder(eng, 1, 11, 95));

  // Clear the crossing ask so the lift below reaches leg A and nothing else.
  eng.submit(InboundCommand{CancelOrder{1, SYM, {}, 2}}, 5);
  ASSERT_FALSE(eng.book().contains(1));

  cap.clear();
  eng.submit(InboundCommand{limit(12, Side::BUY, 110, 1, 3)}, 6);  // lift leg A
  EXPECT_EQ(cap.trades(), 1);

  EXPECT_FALSE(cap.canceled(11, CancelReason::OcoTriggered));
  EXPECT_TRUE(eng.book().contains(11));
  EXPECT_TRUE(hasOpenOrder(eng, 1, 11, 95));
  ASSERT_TRUE(eng.book().bestBid().has_value());
  EXPECT_EQ(eng.book().bestBid().value(), px(95));
}

// Zero-fill residual cancel: an IOC leg that crosses nothing is canceled on
// the raw-sink path, also past the commit point.
TEST(OcoCommitBoundary, AResidualCancelLeavesNoDeadIdInTheGroup)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  NewOrder legA = limit(20, Side::SELL, 110, 1, 1);
  legA.ocoGroup = 7;
  eng.submit(InboundCommand{legA}, 1);  // group 7 = [20], resting

  NewOrder legB = limit(21, Side::BUY, 95, 1, 1, TimeInForce::IOC);
  legB.ocoGroup = 7;
  eng.submit(InboundCommand{legB}, 2);  // nothing to cross: residual canceled
  ASSERT_TRUE(cap.canceled(21, CancelReason::ImmediateOrCancelResidual));
  EXPECT_FALSE(eng.book().contains(21));

  eng.submit(InboundCommand{limit(21, Side::BUY, 95, 1, 1)}, 3);  // REUSE id 21, plain GTC
  ASSERT_TRUE(hasOpenOrder(eng, 1, 21, 95));

  cap.clear();
  eng.submit(InboundCommand{limit(22, Side::BUY, 110, 1, 3)}, 4);  // lift leg A
  EXPECT_EQ(cap.trades(), 1);

  EXPECT_FALSE(cap.canceled(21, CancelReason::OcoTriggered));
  EXPECT_TRUE(eng.book().contains(21));
  EXPECT_TRUE(hasOpenOrder(eng, 1, 21, 95));
}

// LULD breach: the band is read after committed = true, so a leg refused with
// LuldBreach is dead and still linked.
TEST(OcoCommitBoundary, ALuldRefusalLeavesNoDeadIdInTheGroup)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(luldCfg(), cap.sink());

  // A first print gives the band a reference: last = 100 -> [99, 101].
  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 1, 2)}, 1);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 1, 3)}, 2);
  ASSERT_EQ(cap.trades(), 1);

  NewOrder legA = limit(30, Side::SELL, 100.5, 1, 1);
  legA.ocoGroup = 9;
  eng.submit(InboundCommand{legA}, 3);  // group 9 = [30], in band, resting
  ASSERT_TRUE(eng.book().contains(30));

  NewOrder legB = limit(31, Side::BUY, 102, 1, 1);
  legB.ocoGroup = 9;
  eng.submit(InboundCommand{legB}, 4);  // above the band: refused past the commit point
  ASSERT_TRUE(cap.rejected(31, RejectReason::LuldBreach));

  eng.setHalted(false);  // clear the volatility pause the breach tripped

  eng.submit(InboundCommand{limit(31, Side::BUY, 99.5, 1, 1)}, 5);  // REUSE id 31
  ASSERT_TRUE(hasOpenOrder(eng, 1, 31, 99.5));

  cap.clear();
  eng.submit(InboundCommand{limit(32, Side::BUY, 100.5, 1, 3)}, 6);  // lift leg A
  EXPECT_EQ(cap.trades(), 1);

  EXPECT_FALSE(cap.canceled(31, CancelReason::OcoTriggered));
  EXPECT_TRUE(eng.book().contains(31));
  EXPECT_TRUE(hasOpenOrder(eng, 1, 31, 99.5));
}

// ---- the residual cancel after a PARTIAL fill ------------------------------

// The residual-cancel branch is the dead leg's only exit: the order was never
// tracked, so no forgetOrder will ever run for it. Whether it printed first
// changes nothing about that -- a leg that filled part of its size and had
// the rest cancelled is just as gone as one that filled nothing, and just as
// obliged to leave the group on its way out.
TEST(OcoCommitBoundary, APartiallyFilledResidualLeavesTheGroup)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 2, 2)}, 1);  // unrelated liquidity

  NewOrder legA = limit(20, Side::SELL, 110, 1, 1);
  legA.ocoGroup = 7;
  eng.submit(InboundCommand{legA}, 2);  // group 7 = [20], resting
  ASSERT_TRUE(eng.book().contains(20));

  NewOrder legB = limit(21, Side::BUY, 100, 5, 1, TimeInForce::IOC);
  legB.ocoGroup = 7;
  cap.clear();
  eng.submit(InboundCommand{legB}, 3);  // prints 2, the other 3 are cancelled

  EXPECT_EQ(cap.trades(), 1);
  ASSERT_TRUE(cap.canceled(21, CancelReason::ImmediateOrCancelResidual));

  // The print decided the group: the sibling loses, once, and the leg that
  // won is not cancelled for having won.
  EXPECT_EQ(cap.cancelCount(CancelReason::OcoTriggered), 1);
  EXPECT_TRUE(cap.canceled(20, CancelReason::OcoTriggered));
  EXPECT_FALSE(cap.canceled(21, CancelReason::OcoTriggered));
  EXPECT_FALSE(eng.book().contains(20));

  // And the dead id is out of the group: reused, it is an ordinary order that
  // no later print can reach.
  eng.submit(InboundCommand{limit(21, Side::BUY, 95, 1, 1)}, 4);
  ASSERT_TRUE(hasOpenOrder(eng, 1, 21, 95));

  cap.clear();
  eng.submit(InboundCommand{limit(1, Side::SELL, 95, 1, 2)}, 5);  // print against the reused id
  EXPECT_EQ(cap.trades(), 1);
  EXPECT_EQ(cap.cancelCount(CancelReason::OcoTriggered), 0);
  EXPECT_FALSE(cap.canceled(21, CancelReason::OcoTriggered));
}

// The same exit, where the group's OTHER member is the maker the leg printed
// against and is consumed by that print. The sibling is gone and forgotten,
// so the group's last member is the dead leg itself -- and the only thing
// that takes it out is its own unlink on the way through the residual
// cancel. Leave it in and the resolution hands the group's own winner list a
// dead id to cancel: it rests nowhere, so nothing is reported, but the
// cancel path resolves that id's last-look holds first -- and this leg has
// one, taken from a non-firm maker on the same sweep. A hold that belongs to
// a settled print is collateral damage.
TEST(OcoCommitBoundary, APartiallyFilledResidualDoesNotTakeItsOwnHoldDownWithIt)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(lastLookCfg(), cap.sink());

  NewOrder legA = limit(20, Side::SELL, 100, 2, 1);
  legA.ocoGroup = 7;
  eng.submit(InboundCommand{legA}, 1);  // group 7 = [20], firm, best ask

  NewOrder soft = limit(11, Side::SELL, 101, 2, 3);
  soft.lastLook = true;
  eng.submit(InboundCommand{soft}, 2);  // non-firm, one tick behind
  ASSERT_TRUE(eng.book().contains(11));

  NewOrder legB = limit(21, Side::BUY, 101, 6, 1, TimeInForce::IOC);
  legB.ocoGroup = 7;
  cap.clear();
  eng.submit(InboundCommand{legB}, 3);

  // What the sweep did: printed 2 against the sibling (consuming it), held 2
  // against the non-firm maker, cancelled the 2 it could not fill.
  EXPECT_EQ(cap.trades(), 1);
  EXPECT_EQ(cap.count<FillHeld>(), 1);
  ASSERT_TRUE(cap.canceled(21, CancelReason::ImmediateOrCancelResidual));
  EXPECT_FALSE(eng.book().contains(20));

  uint64_t heldId = 0;
  for (const auto& e : cap.ev)
  {
    if (const auto* h = std::get_if<FillHeld>(&e))
    {
      heldId = h->heldId;
    }
  }
  ASSERT_NE(heldId, 0u);

  // The hold is untouched: it belongs to a print that has not been decided
  // yet, and the dead leg's departure from the group is not a cancel of it.
  EXPECT_EQ(cap.count<FillRejected>(), 0);
  EXPECT_EQ(eng.openHolds(), 1u);
  EXPECT_TRUE(eng.hasHold(heldId));
  EXPECT_EQ(cap.cancelCount(CancelReason::OcoTriggered), 0);
  // The non-firm maker's whole size is held out of the book and stays there.
  EXPECT_EQ(eng.book().find(11), nullptr);
}

// ---- controls: green today, and a fix must keep them green -----------------

// The rule the OCO group exists for. A fix that stopped linking (or stopped
// resolving) would pass every test above and break this one.
TEST(OcoCommitBoundary, ControlARestingSiblingStillLosesWhenTheOtherLegFills)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  NewOrder legA = limit(40, Side::SELL, 110, 1, 1);
  legA.ocoGroup = 3;
  NewOrder legB = limit(41, Side::BUY, 90, 1, 1);
  legB.ocoGroup = 3;
  eng.submit(InboundCommand{legA}, 1);
  eng.submit(InboundCommand{legB}, 2);
  ASSERT_TRUE(eng.book().contains(40));
  ASSERT_TRUE(eng.book().contains(41));

  cap.clear();
  eng.submit(InboundCommand{limit(42, Side::BUY, 110, 1, 3)}, 3);  // lift leg A
  EXPECT_EQ(cap.trades(), 1);
  EXPECT_TRUE(cap.canceled(41, CancelReason::OcoTriggered));
  EXPECT_FALSE(eng.book().contains(41));
  EXPECT_TRUE(eng.snapshotAccount(1).openOrders.empty());
}

// A refusal BEFORE the commit point already unlinks, and must keep doing so.
TEST(OcoCommitBoundary, ControlARefusalBeforeTheCommitPointStillUnlinks)
{
  Cap cap;
  SymbolConfig c = cfg();
  c.maxOrderQty = qty(5);  // fat-finger size, checked inside validate()
  MatchingEngine<MatchingBook> eng(c, cap.sink());

  NewOrder legA = limit(50, Side::SELL, 110, 1, 1);
  legA.ocoGroup = 4;
  eng.submit(InboundCommand{legA}, 1);

  NewOrder legB = limit(51, Side::BUY, 90, 99, 1);  // over the cap
  legB.ocoGroup = 4;
  eng.submit(InboundCommand{legB}, 2);
  ASSERT_TRUE(cap.rejected(51, RejectReason::OrderTooLarge));

  eng.submit(InboundCommand{limit(51, Side::BUY, 95, 1, 1)}, 3);  // REUSE id 51
  ASSERT_TRUE(hasOpenOrder(eng, 1, 51, 95));

  cap.clear();
  eng.submit(InboundCommand{limit(52, Side::BUY, 110, 1, 3)}, 4);
  EXPECT_EQ(cap.trades(), 1);
  EXPECT_FALSE(cap.canceled(51, CancelReason::OcoTriggered));
  EXPECT_TRUE(eng.book().contains(51));
}

// A conditional parked in the stop book IS committed -- validate.inl sets
// `committed = onStop(o)` for exactly that reason. A fix that moved the
// commit point past the stop branch would leave a parked leg unlinked and
// this control would go red.
TEST(OcoCommitBoundary, ControlAParkedConditionalKeepsItsOcoLink)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 1, 2)}, 1);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 1, 3)}, 2);  // last price = 100
  ASSERT_EQ(cap.trades(), 1);

  NewOrder legA = limit(60, Side::SELL, 110, 1, 1);
  legA.ocoGroup = 6;
  eng.submit(InboundCommand{legA}, 3);

  NewOrder legB = stopMarket(61, Side::SELL, 1, 50, 1);  // trigger far below: parked
  legB.ocoGroup = 6;
  eng.submit(InboundCommand{legB}, 4);
  ASSERT_EQ(eng.snapshotAccount(1).pendingStops.size(), 1u);

  cap.clear();
  eng.submit(InboundCommand{limit(62, Side::BUY, 110, 1, 3)}, 5);  // lift leg A
  EXPECT_EQ(cap.trades(), 1);
  EXPECT_TRUE(cap.canceled(61, CancelReason::OcoTriggered));
  EXPECT_TRUE(eng.snapshotAccount(1).pendingStops.empty());
}

}  // namespace
