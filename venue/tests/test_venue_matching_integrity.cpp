/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Order-integrity invariants of the matching engine: who may act on an order,
// what a modify must preserve, and what a time-in-force promise is worth when
// another gate cuts liquidity out from under the sweep.
//
// Every case here is a property no other venue test asserts. They are grouped
// by the invariant they defend rather than by the code path that used to break
// it, so a future refactor that moves the path keeps the assertion meaningful.

#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;
constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE = 999;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount quoteAmt(double v) { return amountOf(Volume::fromDouble(v)); }

SymbolConfig spotCfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  return c;
}

SymbolConfig perpCfg()
{
  SymbolConfig c = spotCfg();
  c.linearPerp = true;
  c.initialMarginBps = 1000;  // 10x
  return c;
}

NewOrder limitOrder(OrderId id, Side s, double p, double q, uint64_t acct)
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
  int rejects(RejectReason r) const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<OrderRejected>(&e); x != nullptr && x->reason == r)
      {
        ++n;
      }
    }
    return n;
  }
  int cancelRejects(RejectReason r) const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (const auto* x = std::get_if<CancelRejected>(&e); x != nullptr && x->reason == r)
      {
        ++n;
      }
    }
    return n;
  }
  int cancels(CancelReason r) const
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
  Quantity tradedQty() const
  {
    Quantity q{};
    for (const auto& e : ev)
    {
      if (const auto* t = std::get_if<Trade>(&e); t != nullptr)
      {
        q += t->quantity;
      }
    }
    return q;
  }
  const OrderAccepted* acceptOf(OrderId id) const
  {
    for (const auto& e : ev)
    {
      if (const auto* a = std::get_if<OrderAccepted>(&e); a != nullptr && a->id == id)
      {
        return a;
      }
    }
    return nullptr;
  }
};

// ---------------------------------------------------------------------------
// Ownership: a command addressed at an order id may only come from the account
// that owns that order. Order ids are a single global namespace chosen by the
// client, so without this check the id IS the capability.
// ---------------------------------------------------------------------------

TEST(VenueMatchingIntegrity, ModifyFromAnotherAccountIsRefused)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(100, BASE, amountOf(Volume::fromDouble(10)));
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 10, /*victim*/ 100)}, 0);
  ASSERT_NE(eng.book().find(1), nullptr);
  cap.clear();

  ModifyOrder attack;
  attack.id = 1;
  attack.symbol = SYM;
  attack.newPrice = px(50);
  attack.newQty = qty(10);
  attack.accountId = 666;
  eng.submit(InboundCommand{attack}, 1);

  EXPECT_EQ(cap.cancelRejects(RejectReason::NotOrderOwner), 1);
  EXPECT_EQ(cap.count<OrderModified>(), 0);
  const RestingOrder* r = eng.book().find(1);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->price, px(100));  // untouched
  EXPECT_EQ(r->accountId, 100u);
}

TEST(VenueMatchingIntegrity, CancelFromAnotherAccountIsRefused)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(100, BASE, amountOf(Volume::fromDouble(10)));
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 10, 100)}, 0);
  cap.clear();

  eng.submit(InboundCommand{CancelOrder{1, SYM, {}, /*accountId*/ 666}}, 1);

  EXPECT_EQ(cap.cancelRejects(RejectReason::NotOrderOwner), 1);
  EXPECT_EQ(cap.count<OrderCanceled>(), 0);
  EXPECT_NE(eng.book().find(1), nullptr);
}

TEST(VenueMatchingIntegrity, QuoteCannotSeizeAnotherAccountsOrderId)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(100, BASE, amountOf(Volume::fromDouble(10)));
  led.deposit(666, BASE, amountOf(Volume::fromDouble(10)));
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{limitOrder(7, Side::SELL, 100, 10, 100)}, 0);
  cap.clear();

  Quote q;
  q.bidId = 8;
  q.askId = 7;  // the victim's live order id
  q.symbol = SYM;
  q.askPrice = px(200);
  q.askQty = qty(1);
  q.accountId = 666;
  eng.submit(InboundCommand{q}, 1);

  EXPECT_EQ(cap.rejects(RejectReason::NotOrderOwner), 1);
  const RestingOrder* r = eng.book().find(7);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->accountId, 100u);  // still the victim's
  EXPECT_EQ(r->price, px(100));
}

TEST(VenueMatchingIntegrity, UnboundCallerKeepsActingOnAnyOrder)
{
  // accountId 0 is the documented "unbound / trusted-transport" sentinel that
  // GatewaySession leaves unstamped. An embedder driving the engine in-process
  // must keep working.
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 10, 100)}, 0);
  cap.clear();

  eng.submit(InboundCommand{CancelOrder{1, SYM, {}, /*accountId*/ 0}}, 1);
  EXPECT_EQ(cap.count<OrderCanceled>(), 1);
  EXPECT_EQ(eng.book().find(1), nullptr);
}

// ---------------------------------------------------------------------------
// A modify that ends in a cancel must leave nothing behind.
// ---------------------------------------------------------------------------

TEST(VenueMatchingIntegrity, ModifyCanceledBySelfTradePreventionFreesItsReservation)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(7, BASE, amountOf(Volume::fromDouble(5)));
  led.deposit(7, QUOTE, quoteAmt(100000));
  eng.setLedger(&led, VENUE);

  NewOrder ask = limitOrder(1, Side::SELL, 100, 5, 7);
  ask.stp = STPMode::CancelNewest;
  eng.submit(InboundCommand{ask}, 0);
  NewOrder bid = limitOrder(2, Side::BUY, 90, 5, 7);
  bid.stp = STPMode::CancelNewest;
  eng.submit(InboundCommand{bid}, 1);

  const Amount reservedBefore = led.reserved(7, QUOTE);
  EXPECT_EQ(reservedBefore, quoteAmt(450));
  cap.clear();

  // Reprice the bid into its own ask: STP kills the re-entering aggressor.
  ModifyOrder m;
  m.id = 2;
  m.symbol = SYM;
  m.newPrice = px(100);
  m.newQty = qty(5);
  m.accountId = 7;
  eng.submit(InboundCommand{m}, 2);

  EXPECT_EQ(eng.book().find(2), nullptr);
  EXPECT_EQ(cap.cancels(CancelReason::SelfTradePrevention), 1);  // the client is told
  EXPECT_EQ(led.reserved(7, QUOTE), Amount{0});                  // nothing frozen
  EXPECT_EQ(eng.restingOrderCount(), 1u);                        // no phantom in the open-order count

  // And the phantom no longer answers as a live order.
  cap.clear();
  eng.submit(InboundCommand{CancelOrder{2, SYM, {}, 7}}, 3);
  EXPECT_EQ(cap.cancelRejects(RejectReason::UnknownOrder), 1);
}

// ---------------------------------------------------------------------------
// Fill-or-kill is all-or-none under every policy and every other gate.
// ---------------------------------------------------------------------------

TEST(VenueMatchingIntegrity, ProRataFillOrKillNeverRests)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink(), MatchingBook{}, MatchPolicy::ProRata);
  Ledger led;
  led.deposit(7, BASE, amountOf(Volume::fromDouble(10)));
  led.deposit(7, QUOTE, quoteAmt(100000));
  eng.setLedger(&led, VENUE);

  NewOrder maker = limitOrder(1, Side::SELL, 100, 5, 7);
  eng.submit(InboundCommand{maker}, 0);
  cap.clear();

  NewOrder fok = limitOrder(9, Side::BUY, 100, 5, 7);
  fok.tif = TimeInForce::FOK;
  fok.stp = STPMode::CancelOldest;
  eng.submit(InboundCommand{fok}, 1);

  EXPECT_EQ(eng.book().find(9), nullptr);
  EXPECT_EQ(cap.tradedQty(), Quantity{});
  EXPECT_EQ(cap.rejects(RejectReason::FillOrKillUnfulfillable), 1);
}

TEST(VenueMatchingIntegrity, FillOrKillIsNotPartiallyPrintedWhenAMakerIsRiskBlocked)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(perpCfg(), cap.sink());
  Ledger led;
  led.deposit(1, QUOTE, quoteAmt(1000000));
  led.deposit(2, QUOTE, quoteAmt(1000000));
  led.deposit(3, QUOTE, quoteAmt(1000000));
  eng.setLedger(&led, VENUE);

  // Account 1 opens a long 10.
  eng.submit(InboundCommand{limitOrder(1, Side::BUY, 100, 10, 1)}, 0);
  eng.submit(InboundCommand{limitOrder(2, Side::SELL, 100, 10, 2)}, 1);
  ASSERT_EQ(eng.positionQty(1), qty(10).raw());

  // Two reduce-only sells of 10 each against a long of 10. Only 10 of that is
  // genuinely reducible, so the book must never show 20 of reduce-only depth.
  NewOrder ro1 = limitOrder(3, Side::SELL, 101, 10, 1);
  ro1.reduceOnly = true;
  eng.submit(InboundCommand{ro1}, 2);
  NewOrder ro2 = limitOrder(4, Side::SELL, 101, 10, 1);
  ro2.reduceOnly = true;
  eng.submit(InboundCommand{ro2}, 3);

  Quantity restingReduceOnly{};
  eng.book().forEachOrder(
      [&](const RestingOrder& o)
      {
        if (o.reduceOnly)
        {
          restingReduceOnly += o.leaves + o.hidden;
        }
      });
  EXPECT_EQ(restingReduceOnly, qty(10));  // not 20

  cap.clear();
  NewOrder fok = limitOrder(9, Side::BUY, 102, 20, 3);
  fok.tif = TimeInForce::FOK;
  eng.submit(InboundCommand{fok}, 4);

  // All-or-none: either 20 printed or nothing did. Half is the defect.
  EXPECT_EQ(cap.tradedQty(), Quantity{});
  EXPECT_EQ(eng.positionQty(3), 0);
  EXPECT_EQ(eng.book().find(9), nullptr);
}

// ---------------------------------------------------------------------------
// A modify preserves the controls the order was admitted with.
// ---------------------------------------------------------------------------

TEST(VenueMatchingIntegrity, RepricedIcebergKeepsItsHiddenReserve)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(1, BASE, amountOf(Volume::fromDouble(1000)));
  eng.setLedger(&led, VENUE);

  NewOrder ice = limitOrder(1, Side::SELL, 100, 100, 1);
  ice.visibleQuantity = qty(10);
  eng.submit(InboundCommand{ice}, 0);
  {
    const RestingOrder* r = eng.book().find(1);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(r->leaves, qty(10));
    ASSERT_EQ(r->hidden, qty(90));
  }

  ModifyOrder m;
  m.id = 1;
  m.symbol = SYM;
  m.newPrice = px(100.01);
  m.newQty = qty(100);
  m.accountId = 1;
  eng.submit(InboundCommand{m}, 1);

  const RestingOrder* r = eng.book().find(1);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->price, px(100.01));
  EXPECT_EQ(r->peak, qty(10));
  EXPECT_EQ(r->leaves, qty(10));  // still shows only the peak
  EXPECT_EQ(r->hidden, qty(90));  // 90 units did not become public
}

TEST(VenueMatchingIntegrity, AmendedPostOnlyOrderStillRefusesToTake)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(1, BASE, amountOf(Volume::fromDouble(100)));
  led.deposit(2, QUOTE, quoteAmt(100000));
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 5, 1)}, 0);

  NewOrder po = limitOrder(2, Side::BUY, 99, 5, 2);
  po.postOnly = true;
  eng.submit(InboundCommand{po}, 1);
  ASSERT_NE(eng.book().find(2), nullptr);
  cap.clear();

  ModifyOrder m;
  m.id = 2;
  m.symbol = SYM;
  m.newPrice = px(100);  // into the resting offer
  m.newQty = qty(5);
  m.accountId = 2;
  eng.submit(InboundCommand{m}, 2);

  EXPECT_EQ(cap.tradedQty(), Quantity{});
  EXPECT_EQ(cap.rejects(RejectReason::PostOnlyWouldCross), 1);
  EXPECT_NE(eng.book().find(1), nullptr);  // the maker survives
}

TEST(VenueMatchingIntegrity, PostOnlyTimeInForceIsHonouredLikeTheFlag)
{
  // POST_ONLY is spelled two ways. Both must mean the same thing.
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(1, BASE, amountOf(Volume::fromDouble(100)));
  led.deposit(2, QUOTE, quoteAmt(100000));
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 5, 1)}, 0);
  cap.clear();

  NewOrder po = limitOrder(2, Side::BUY, 100, 5, 2);
  po.tif = TimeInForce::POST_ONLY;  // the flag is deliberately left unset
  eng.submit(InboundCommand{po}, 1);

  EXPECT_EQ(cap.tradedQty(), Quantity{});
  EXPECT_EQ(cap.rejects(RejectReason::PostOnlyWouldCross), 1);
}

// ---------------------------------------------------------------------------
// The fat-finger notional gate reads the symbol's own scale.
// ---------------------------------------------------------------------------

TEST(VenueMatchingIntegrity, FatFingerNotionalHonoursPerSymbolScale)
{
  SymbolConfig c = spotCfg();
  c.priceScale = 100;
  c.qtyScale = 100;
  c.tickSize = Price::fromRaw(1);
  c.minPrice = Price::fromRaw(1);
  c.maxPrice = Price::fromRaw(100000000);
  c.maxOrderNotional = Volume::fromDouble(100);

  Cap cap;
  MatchingEngine<MatchingBook> eng(c, cap.sink());

  NewOrder o;
  o.id = 1;
  o.symbol = SYM;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromRaw(100000);       // 1000.00 at priceScale 100
  o.quantity = Quantity::fromRaw(10000);  // 100.00 at qtyScale 100
  o.accountId = 1;                        // true notional 100000, limit 100
  eng.submit(InboundCommand{o}, 0);

  EXPECT_EQ(cap.rejects(RejectReason::OrderTooLarge), 1);
  EXPECT_EQ(eng.book().find(1), nullptr);
}

TEST(VenueMatchingIntegrity, FatFingerNotionalStillPassesAnInBandOrder)
{
  SymbolConfig c = spotCfg();
  c.priceScale = 100;
  c.qtyScale = 100;
  c.tickSize = Price::fromRaw(1);
  c.minPrice = Price::fromRaw(1);
  c.maxPrice = Price::fromRaw(100000000);
  c.maxOrderNotional = Volume::fromDouble(100);

  Cap cap;
  MatchingEngine<MatchingBook> eng(c, cap.sink());

  NewOrder o;
  o.id = 1;
  o.symbol = SYM;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromRaw(1000);       // 10.00
  o.quantity = Quantity::fromRaw(500);  // 5.00 -> notional 50
  o.accountId = 1;
  eng.submit(InboundCommand{o}, 0);

  EXPECT_EQ(cap.rejects(RejectReason::OrderTooLarge), 0);
  EXPECT_NE(eng.book().find(1), nullptr);
}

// ---------------------------------------------------------------------------
// The auction accumulates orders with the same attributes continuous trading
// gives them.
// ---------------------------------------------------------------------------

TEST(VenueMatchingIntegrity, IcebergSubmittedInAnAuctionShowsOnlyItsPeak)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(1, BASE, amountOf(Volume::fromDouble(1000)));
  eng.setLedger(&led, VENUE);
  eng.beginPreOpen();

  NewOrder ice = limitOrder(1, Side::SELL, 100, 100, 1);
  ice.visibleQuantity = qty(10);
  eng.submit(InboundCommand{ice}, 0);

  const RestingOrder* r = eng.book().find(1);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->peak, qty(10));
  EXPECT_EQ(r->leaves, qty(10));
  EXPECT_EQ(r->hidden, qty(90));

  const OrderAccepted* a = cap.acceptOf(1);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->displayQty, qty(10));  // 0 would mean "publish leavesQty"
}

TEST(VenueMatchingIntegrity, GoodTillDateSubmittedInAnAuctionStillExpires)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(1, BASE, amountOf(Volume::fromDouble(1000)));
  eng.setLedger(&led, VENUE);
  eng.beginPreOpen();

  NewOrder o = limitOrder(5, Side::SELL, 100, 10, 1);
  o.tif = TimeInForce::GTD;
  o.expiryNs = SeqNanos::fromRaw(50);
  eng.submit(InboundCommand{o}, 10);
  ASSERT_NE(eng.book().find(5), nullptr);

  eng.submit(InboundCommand{TimeTick{}}, 100000);
  EXPECT_EQ(eng.book().find(5), nullptr);
}

// ---------------------------------------------------------------------------
// Self-trade prevention measures an iceberg by what it actually holds.
// ---------------------------------------------------------------------------

TEST(VenueMatchingIntegrity, DecrementMeasuresAnIcebergByItsWholeSize)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(1, BASE, amountOf(Volume::fromDouble(100)));
  led.deposit(1, QUOTE, quoteAmt(100000));
  eng.setLedger(&led, VENUE);

  NewOrder ice = limitOrder(1, Side::SELL, 100, 100, 1);
  ice.visibleQuantity = qty(10);
  eng.submit(InboundCommand{ice}, 0);
  ASSERT_EQ(led.reserved(1, BASE), amountOf(Volume::fromDouble(100)));
  cap.clear();

  NewOrder aggressor = limitOrder(2, Side::BUY, 100, 50, 1);
  aggressor.stp = STPMode::Decrement;
  eng.submit(InboundCommand{aggressor}, 1);

  // Decrement cancels the smaller leg and trims the larger. The iceberg holds
  // 100 against an incoming 50, so the iceberg is the larger leg.
  const RestingOrder* r = eng.book().find(1);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->leaves + r->hidden, qty(50));
  EXPECT_EQ(eng.book().find(2), nullptr);  // the aggressor never rests
  EXPECT_EQ(led.reserved(1, BASE), amountOf(Volume::fromDouble(50)));
  EXPECT_EQ(cap.tradedQty(), Quantity{});
}

// ---------------------------------------------------------------------------
// A two-sided quote can ask for the same controls a single order can.
// ---------------------------------------------------------------------------

TEST(VenueMatchingIntegrity, QuoteCanBePostOnly)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(2, BASE, amountOf(Volume::fromDouble(100)));
  led.deposit(7, BASE, amountOf(Volume::fromDouble(100)));
  led.deposit(7, QUOTE, quoteAmt(100000));
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 5, 2)}, 0);
  cap.clear();

  Quote q;
  q.bidId = 10;
  q.symbol = SYM;
  q.bidPrice = px(100);  // would lift the resting offer
  q.bidQty = qty(5);
  q.accountId = 7;
  q.postOnly = true;
  eng.submit(InboundCommand{q}, 1);

  EXPECT_EQ(cap.tradedQty(), Quantity{});
  EXPECT_EQ(cap.rejects(RejectReason::PostOnlyWouldCross), 1);
}

TEST(VenueMatchingIntegrity, QuoteCarriesVisibleQuantityAndTimeInForce)
{
  Cap cap;
  MatchingEngine<MatchingBook> eng(spotCfg(), cap.sink());
  Ledger led;
  led.deposit(7, BASE, amountOf(Volume::fromDouble(1000)));
  eng.setLedger(&led, VENUE);

  Quote q;
  q.askId = 11;
  q.symbol = SYM;
  q.askPrice = px(100);
  q.askQty = qty(100);
  q.accountId = 7;
  q.visibleQuantity = qty(10);
  q.tif = TimeInForce::GTD;
  q.expiryNs = SeqNanos::fromRaw(50);
  eng.submit(InboundCommand{q}, 10);

  const RestingOrder* r = eng.book().find(11);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->peak, qty(10));
  EXPECT_EQ(r->hidden, qty(90));

  eng.submit(InboundCommand{TimeTick{}}, 100000);
  EXPECT_EQ(eng.book().find(11), nullptr);
}

// ---------------------------------------------------------------------------
// All-or-none is decided once, before anything prints.
//
// These drive the matcher directly, because the thing under test is what
// happens when the risk answer CHANGES between the precheck and the sweep.
// Inside one shard nothing else runs in that gap, so the only way to stage the
// gap is to make the two answers differ on purpose -- which is exactly the
// shape of the hazard: a liquidation, a funding settlement or a close from
// another instrument moving the position out from under a sweep in flight.
// ---------------------------------------------------------------------------

namespace
{

RestingOrder resting(OrderId id, uint64_t acct, double price, double q)
{
  RestingOrder r;
  r.id = id;
  r.accountId = acct;
  r.price = px(price);
  r.leaves = qty(q);
  return r;
}

// A position that the sweep's own prints move, plus a one-off jolt from
// somewhere else entirely, applied the moment the first print lands.
struct ShiftingPosition
{
  int64_t makerPos = 0;  // raw contracts held by the maker account
  int64_t externalJolt = 0;
  bool jolted = false;

  // How much of a prospective bite the maker's reduce-only limit allows,
  // against the position as it stands right now.
  int64_t allow(int64_t want) const
  {
    const int64_t reducible = (makerPos > 0) ? makerPos : 0;
    return std::min(want, reducible);
  }
};

}  // namespace

TEST(VenueMatchingIntegrity, FillOrKillPrintsNothingWhenThePositionMovesMidSweep)
{
  MatchingBook book;
  // Two reduce-only sells of 10, one account, against a long of 20. Every
  // contract of that depth is genuinely tradeable when the order arrives.
  (void)book.addResting(Side::SELL, resting(1, 5, 100.0, 10));
  (void)book.addResting(Side::SELL, resting(2, 5, 100.0, 10));

  ShiftingPosition state;
  state.makerPos = qty(20).raw();
  state.externalJolt = qty(10).raw();  // half the position vanishes elsewhere

  Matcher<MatchingBook> matcher;
  // The precheck's view: the position as it stands, plus what the sweep has
  // already simulated. This is the answer the plan is built from.
  matcher.setFillLimitDryHook(
      [&](const RestingOrder& maker, const NewOrder&, Quantity want,
          const PositionDeltas& deltas)
      {
        ShiftingPosition asPlanned = state;
        asPlanned.makerPos += deltas.of(maker.accountId);
        const int64_t allowed = asPlanned.allow(want.raw());
        FillLimit lim;
        lim.qty = Quantity::fromRaw(allowed);
        lim.makerQty = lim.qty;
        lim.takerQty = want;
        lim.makerBlocked = (allowed <= 0);
        lim.reason = CancelReason::ReduceOnlyNotReducing;
        return lim;
      });
  // The sweep's view: live state, which the jolt has moved by the time the
  // second maker is measured.
  matcher.setFillLimitHook(
      [&](const RestingOrder&, const NewOrder&, Quantity want)
      {
        const int64_t allowed = state.allow(want.raw());
        FillLimit lim;
        lim.qty = Quantity::fromRaw(allowed);
        lim.makerQty = lim.qty;
        lim.takerQty = want;
        lim.makerBlocked = (allowed <= 0);
        lim.reason = CancelReason::ReduceOnlyNotReducing;
        return lim;
      });

  NewOrder fok = limitOrder(9, Side::BUY, 100.0, 20, 6);
  fok.tif = TimeInForce::FOK;

  Quantity printed{};
  uint64_t tradeId = 0;
  const MatchOutcome out = matcher.cross(
      fok, book, [&]()
      { return ++tradeId; },
      [&](const OutboundEvent& e)
      {
        if (const auto* t = std::get_if<Trade>(&e))
        {
          printed += t->quantity;
          state.makerPos -= t->quantity.raw();  // the sweep's own print
          if (!state.jolted)
          {
            state.jolted = true;
            state.makerPos -= state.externalJolt;  // and the one from outside
          }
        }
      });

  // All-or-none: 20 or nothing. Half is the outcome the promise rules out.
  EXPECT_TRUE(printed.isZero() || printed == qty(20))
      << "printed " << printed.raw() << " of " << fok.quantity.raw();
  EXPECT_EQ(printed, qty(20));  // the depth was there when the order arrived
  EXPECT_EQ(out.filled, qty(20));
  EXPECT_TRUE(out.takerComplete);
}

TEST(VenueMatchingIntegrity, FillOrKillStillRefusesWhenTheDepthWasNeverThere)
{
  MatchingBook book;
  (void)book.addResting(Side::SELL, resting(1, 5, 100.0, 10));
  (void)book.addResting(Side::SELL, resting(2, 5, 100.0, 10));

  ShiftingPosition state;
  state.makerPos = qty(10).raw();  // only 10 of the 20 is reducible

  Matcher<MatchingBook> matcher;
  matcher.setFillLimitDryHook(
      [&](const RestingOrder& maker, const NewOrder&, Quantity want,
          const PositionDeltas& deltas)
      {
        ShiftingPosition asPlanned = state;
        asPlanned.makerPos += deltas.of(maker.accountId);
        const int64_t allowed = asPlanned.allow(want.raw());
        FillLimit lim;
        lim.qty = Quantity::fromRaw(allowed);
        lim.makerQty = lim.qty;
        lim.takerQty = want;
        lim.makerBlocked = (allowed <= 0);
        lim.reason = CancelReason::ReduceOnlyNotReducing;
        return lim;
      });

  NewOrder fok = limitOrder(9, Side::BUY, 100.0, 20, 6);
  fok.tif = TimeInForce::FOK;

  Quantity printed{};
  uint64_t tradeId = 0;
  const MatchOutcome out = matcher.cross(
      fok, book, [&]()
      { return ++tradeId; },
      [&](const OutboundEvent& e)
      {
        if (const auto* t = std::get_if<Trade>(&e))
        {
          printed += t->quantity;
        }
      });

  EXPECT_EQ(out.reject, RejectReason::FillOrKillUnfulfillable);
  EXPECT_TRUE(printed.isZero());
  EXPECT_EQ(matcher.fillOrKillRejected(), 1u);
  EXPECT_EQ(matcher.fillOrKillRiskConstrained(), 1u);
}

}  // namespace
