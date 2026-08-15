/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox/book/ladder_book.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
const char* g_label = "";

void checkImpl(bool ok, const char* expr, const char* file, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL [%s] %s:%d  %s\n", g_label, file, line, expr);
  }
}
#define CHECK(x) checkImpl((x), #x, __FILE__, __LINE__)

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

constexpr SymbolId SYM = 1;

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(0.01);
  c.maxPrice = px(119);
  return c;
}

LadderBook::Config ladderCfg()
{
  return LadderBook::Config{/*base*/ 0, /*tick*/ px(0.01).raw(), /*levels*/ 12000,
                            /*maxOrders*/ 1024};
}

struct Capture
{
  std::vector<OutboundEvent> ev;
  EventSink sink()
  {
    return [this](const OutboundEvent& e)
    { ev.push_back(e); };
  }

  int trades() const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (std::get_if<Trade>(&e))
      {
        ++n;
      }
    }
    return n;
  }
  const Trade* firstTrade() const
  {
    for (const auto& e : ev)
    {
      if (const auto* t = std::get_if<Trade>(&e))
      {
        return t;
      }
    }
    return nullptr;
  }
  // Cancel and replace refusals are their own event: FIX answers them with
  // 35=9, not an execution report, so the engine reports them separately too.
  int cancelRejects(RejectReason r) const
  {
    int n = 0;
    for (auto& e : ev)
    {
      if (auto* x = std::get_if<CancelRejected>(&e); x && x->reason == r)
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
      if (const auto* x = std::get_if<OrderRejected>(&e); x && x->reason == r)
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
      if (const auto* x = std::get_if<OrderCanceled>(&e); x && x->reason == r)
      {
        ++n;
      }
    }
    return n;
  }
  int accepts() const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (std::get_if<OrderAccepted>(&e))
      {
        ++n;
      }
    }
    return n;
  }
  int triggers() const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (std::get_if<OrderTriggered>(&e))
      {
        ++n;
      }
    }
    return n;
  }
  int modifies() const
  {
    int n = 0;
    for (const auto& e : ev)
    {
      if (std::get_if<OrderModified>(&e))
      {
        ++n;
      }
    }
    return n;
  }
};

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct = 1,
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

NewOrder market(OrderId id, Side s, double q, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::MARKET;
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}

NewOrder iceberg(OrderId id, Side s, double p, double total, double vis, uint64_t acct = 1)
{
  NewOrder o = limit(id, s, p, total, acct);
  o.visibleQuantity = qty(vis);
  return o;
}

Quantity tradedTotal(const Capture& cap)
{
  Quantity t{};
  for (const auto& e : cap.ev)
  {
    if (const auto* x = std::get_if<Trade>(&e))
    {
      t += x->quantity;
    }
  }
  return t;
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

NewOrder trailingStop(OrderId id, Side s, double q, double offset, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::TRAILING_STOP;
  o.quantity = qty(q);
  o.trailingOffset = px(offset);
  o.accountId = acct;
  return o;
}

bool tradedBy(const Capture& cap, OrderId taker, double price)
{
  for (const auto& e : cap.ev)
  {
    if (const auto* t = std::get_if<Trade>(&e))
    {
      if (t->takerId == taker && t->price == px(price))
      {
        return true;
      }
    }
  }
  return false;
}

template <class Book>
void runAll(const std::function<Book()>& mk, const char* label)
{
  g_label = label;
  std::printf("== book: %s ==\n", label);

  {  // basic cross
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 1));
    CHECK(cap.accepts() == 1);
    CHECK(cap.trades() == 0);
    eng.submit(limit(2, Side::BUY, 100, 3, 2));
    CHECK(cap.trades() == 1);
    CHECK(cap.firstTrade() && cap.firstTrade()->quantity == qty(3));
    CHECK(cap.firstTrade() && cap.firstTrade()->price == px(100));
    CHECK(cap.firstTrade() && cap.firstTrade()->makerId == 1 && cap.firstTrade()->takerId == 2);
    CHECK(eng.tradesGenerated() == 1);
  }
  {  // price-time priority: lower ask fills first, remainder sweeps up
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 2, 1));
    eng.submit(limit(2, Side::SELL, 101, 2, 1));
    eng.submit(limit(3, Side::BUY, 101, 3, 2));  // takes 2@100 then 1@101
    CHECK(cap.trades() == 2);
    // first fill at the better (lower) ask
    CHECK(cap.firstTrade() && cap.firstTrade()->price == px(100));
    CHECK(eng.book().bestAsk().has_value() && eng.book().bestAsk().value() == px(101));
  }
  {  // FIFO within a level: earlier order at same price fills first
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(10, Side::SELL, 100, 2, 1));
    eng.submit(limit(11, Side::SELL, 100, 2, 1));
    eng.submit(limit(12, Side::BUY, 100, 1, 2));
    CHECK(cap.firstTrade() && cap.firstTrade()->makerId == 10);
  }
  {  // market residual canceled
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 1));
    eng.submit(market(2, Side::BUY, 8, 2));
    CHECK(cap.trades() == 1);
    CHECK(cap.firstTrade() && cap.firstTrade()->quantity == qty(5));
    CHECK(cap.cancels(CancelReason::MarketResidual) == 1);
  }
  {  // post-only reject
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 1));
    NewOrder p = limit(2, Side::BUY, 101, 2, 2);
    p.postOnly = true;
    eng.submit(p);
    CHECK(cap.trades() == 0);
    CHECK(cap.rejects(RejectReason::PostOnlyWouldCross) == 1);
  }
  {  // FOK
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 1));
    eng.submit(limit(2, Side::BUY, 100, 10, 2, TimeInForce::FOK));
    CHECK(cap.trades() == 0);
    CHECK(cap.rejects(RejectReason::FillOrKillUnfulfillable) == 1);
    eng.submit(limit(3, Side::BUY, 100, 4, 2, TimeInForce::FOK));
    CHECK(cap.trades() == 1);
    CHECK(cap.firstTrade() && cap.firstTrade()->quantity == qty(4));
  }
  {  // IOC residual canceled
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 2, 1));
    eng.submit(limit(2, Side::BUY, 100, 5, 2, TimeInForce::IOC));
    CHECK(cap.trades() == 1);
    CHECK(cap.firstTrade() && cap.firstTrade()->quantity == qty(2));
    CHECK(cap.cancels(CancelReason::ImmediateOrCancelResidual) == 1);
    CHECK(cap.accepts() == 1);
  }
  {  // STP cancel-newest
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 7));
    NewOrder o = limit(2, Side::BUY, 100, 3, 7);
    o.stp = STPMode::CancelNewest;
    eng.submit(o);
    CHECK(cap.trades() == 0);
    CHECK(cap.cancels(CancelReason::SelfTradePrevention) == 1);
  }
  {  // cancel + unknown-order reject
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 1));
    eng.submit(CancelOrder{1, SYM, 1});
    CHECK(cap.cancels(CancelReason::UserRequested) == 1);
    CHECK(eng.book().empty());
    eng.submit(CancelOrder{999, SYM, 1});
    CHECK(cap.cancelRejects(RejectReason::UnknownOrder) == 1);
    CHECK(cap.rejects(RejectReason::UnknownOrder) == 0);  // not an exec report
  }
  {  // duplicate order id reject
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 1));
    eng.submit(limit(1, Side::SELL, 101, 5, 1));
    CHECK(cap.rejects(RejectReason::DuplicateOrderId) == 1);
  }
  {  // modify: shrink at same price keeps priority (in place, no trade)
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 1));
    eng.submit(ModifyOrder{1, SYM, Price{}, qty(2), 1});  // keep price, shrink 5->2
    CHECK(cap.trades() == 0);
    // only 2 left resting: a buy for 3 takes 2 and leaves 1 unmatched
    eng.submit(limit(2, Side::BUY, 100, 3, 2));
    CHECK(cap.trades() == 1);
    CHECK(cap.firstTrade() && cap.firstTrade()->quantity == qty(2));
  }
  {  // modify: reprice into a cross matches immediately (loses priority)
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::BUY, 99, 5, 1));    // resting bid @99
    eng.submit(limit(2, Side::SELL, 100, 5, 2));  // resting ask @100 (no cross)
    CHECK(cap.trades() == 0);
    eng.submit(ModifyOrder{1, SYM, px(100), qty(5), 1});  // reprice bid 99->100, crosses ask
    CHECK(cap.trades() == 1);
    CHECK(cap.firstTrade() && cap.firstTrade()->price == px(100));
  }
  {  // modify unknown order rejects
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(ModifyOrder{42, SYM, px(100), qty(1), 1});
    CHECK(cap.cancelRejects(RejectReason::UnknownOrder) == 1);
    CHECK(cap.rejects(RejectReason::UnknownOrder) == 0);
  }
  {  // iceberg fully consumed via peak refills
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(iceberg(1, Side::SELL, 100, 10, 2, 1));  // total 10, peak 2
    eng.submit(limit(2, Side::BUY, 100, 10, 2));
    CHECK(cap.trades() == 5);  // 5 refilled bites of 2
    CHECK(tradedTotal(cap) == qty(10));
    CHECK(eng.book().empty());
  }
  {  // iceberg refill loses priority to a same-price displayed order
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(iceberg(1, Side::SELL, 100, 10, 2, 1));
    eng.submit(limit(2, Side::SELL, 100, 2, 1));
    eng.submit(limit(3, Side::BUY, 100, 4, 2));
    CHECK(cap.trades() == 2);
    std::vector<OrderId> makers;
    for (const auto& e : cap.ev)
    {
      if (const auto* t = std::get_if<Trade>(&e))
      {
        makers.push_back(t->makerId);
      }
    }
    CHECK(makers.size() == 2 && makers[0] == 1 && makers[1] == 2);  // id1 peak, then id2
  }
  {  // FOK counts the iceberg hidden reserve as available liquidity
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(iceberg(1, Side::SELL, 100, 10, 2, 1));
    eng.submit(limit(2, Side::BUY, 100, 8, 2, TimeInForce::FOK));
    CHECK(cap.rejects(RejectReason::FillOrKillUnfulfillable) == 0);
    CHECK(tradedTotal(cap) == qty(8));
  }
  {  // buy stop-market fires when the last price rises to the trigger
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 1));
    eng.submit(limit(2, Side::BUY, 100, 5, 2));  // trade @100 -> last=100
    eng.submit(stopMarket(3, Side::BUY, 2, 105, 2));
    CHECK(cap.trades() == 1);
    CHECK(cap.triggers() == 0);  // 100 < 105, still pending
    eng.submit(limit(4, Side::SELL, 105, 10, 1));
    eng.submit(limit(5, Side::BUY, 105, 1, 2));  // trade @105 -> last=105 -> stop fires
    CHECK(cap.triggers() == 1);
    CHECK(tradedBy(cap, 3, 105.0));  // the triggered stop (taker 3) traded at 105
  }
  {  // sell trailing-stop ratchets up, then fires on the pullback
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 1, 1));
    eng.submit(limit(2, Side::BUY, 100, 1, 2));        // trade @100 -> last=100
    eng.submit(trailingStop(3, Side::SELL, 2, 5, 1));  // trigger starts 95
    eng.submit(limit(4, Side::SELL, 110, 1, 5));
    eng.submit(limit(5, Side::BUY, 110, 1, 6));  // trade @110 -> trigger ratchets to 105
    CHECK(cap.triggers() == 0);                  // 110 > 105, not fired
    eng.submit(limit(6, Side::BUY, 105, 10, 7));
    eng.submit(limit(7, Side::SELL, 105, 1, 8));  // trade @105 -> last<=trigger -> fires
    CHECK(cap.triggers() == 1);
    CHECK(tradedBy(cap, 3, 105.0));
  }
  {  // STP Decrement: incoming smaller -> reduce resting, cancel incoming, no trade
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 7));
    NewOrder o = limit(2, Side::BUY, 100, 3, 7);
    o.stp = STPMode::Decrement;
    eng.submit(o);
    CHECK(cap.trades() == 0);
    CHECK(cap.cancels(CancelReason::SelfTradePrevention) == 1);
    eng.submit(limit(3, Side::BUY, 100, 5, 2));  // resting was reduced 5->2
    CHECK(cap.trades() == 1);
    CHECK(cap.firstTrade() && cap.firstTrade()->quantity == qty(2));
  }
  {  // STP Decrement: incoming larger -> cancel resting, incoming rests
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 3, 7));
    NewOrder o = limit(2, Side::BUY, 100, 5, 7);
    o.stp = STPMode::Decrement;
    eng.submit(o);
    CHECK(cap.trades() == 0);
    CHECK(cap.cancels(CancelReason::SelfTradePrevention) == 1);
    CHECK(cap.accepts() == 2);  // resting sell (later STP-canceled) + incoming residual rests
  }
  {  // fat-finger: max order qty
    Capture cap;
    auto fc = cfg();
    fc.maxOrderQty = qty(100);
    MatchingEngine<Book> eng(fc, cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 101, 1));  // 101 > 100
    CHECK(cap.rejects(RejectReason::OrderTooLarge) == 1);
  }
  {  // fat-finger: max notional
    Capture cap;
    auto fc = cfg();
    fc.maxOrderNotional = px(100) * qty(10);  // 1000
    MatchingEngine<Book> eng(fc, cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 20, 1));  // notional 2000 > 1000
    CHECK(cap.rejects(RejectReason::OrderTooLarge) == 1);
    eng.submit(limit(2, Side::SELL, 100, 5, 1));  // notional 500 ok
    CHECK(cap.accepts() == 1);
  }
  {  // max open orders per account: cap live resting orders, freed by cancel
    Capture cap;
    auto fc = cfg();
    fc.maxOpenOrders = 2;
    MatchingEngine<Book> eng(fc, cap.sink(), mk());
    eng.submit(limit(1, Side::BUY, 99, 1, 5));  // rests (1 open)
    eng.submit(limit(2, Side::BUY, 98, 1, 5));  // rests (2 open)
    eng.submit(limit(3, Side::BUY, 97, 1, 5));  // at cap -> rejected
    CHECK(cap.rejects(RejectReason::TooManyOpenOrders) == 1);
    CHECK(cap.accepts() == 2);
    eng.submit(CancelOrder{1, SYM, 5});         // free a slot
    eng.submit(limit(4, Side::BUY, 96, 1, 5));  // now fits
    CHECK(cap.accepts() == 3);
    CHECK(cap.rejects(RejectReason::TooManyOpenOrders) == 1);  // still just the one
    // A different account is unaffected by account 5's count.
    eng.submit(limit(5, Side::BUY, 95, 1, 6));
    CHECK(cap.accepts() == 4);
  }
  {  // GTD: a resting order auto-cancels once its expiry sequencer-ts passes
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    NewOrder o = limit(1, Side::BUY, 99, 5, 1);
    o.tif = TimeInForce::GTD;
    o.expiryNs = 1000;
    eng.submit(InboundCommand{o}, 100);  // rests at t=100
    CHECK(cap.accepts() == 1);
    eng.submit(InboundCommand{limit(2, Side::BUY, 98, 1, 2)}, 500);  // t=500, not expired
    CHECK(!eng.book().empty());
    CHECK(cap.cancels(CancelReason::Expired) == 0);
    // A submit at t >= 1000 sweeps the expired GTD order before processing.
    eng.submit(InboundCommand{limit(3, Side::BUY, 97, 1, 3)}, 1000);
    CHECK(cap.cancels(CancelReason::Expired) == 1);
    CHECK(eng.book().bestBid().value() == px(98));  // 99 gone, best is now 98
    // GTC orders never expire.
    Capture cap2;
    MatchingEngine<Book> eng2(cfg(), cap2.sink(), mk());
    eng2.submit(InboundCommand{limit(1, Side::BUY, 99, 5, 1)}, 100);
    eng2.submit(InboundCommand{limit(2, Side::SELL, 105, 1, 2)}, 1000000);
    CHECK(cap2.cancels(CancelReason::Expired) == 0);
  }
  {  // OCO: two resting sells linked; a fill on one cancels the other
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    NewOrder tp = limit(1, Side::SELL, 102, 5, 1);  // take-profit
    tp.ocoGroup = 77;
    NewOrder sl = limit(2, Side::SELL, 105, 5, 1);  // the linked sibling
    sl.ocoGroup = 77;
    eng.submit(tp);
    eng.submit(sl);
    CHECK(cap.accepts() == 2);
    // A buy lifts the 102 sell -> that OCO order fills, its sibling (105) cancels.
    eng.submit(limit(3, Side::BUY, 102, 5, 2));
    CHECK(cap.trades() == 1);
    CHECK(cap.cancels(CancelReason::OcoTriggered) == 1);
    CHECK(eng.book().bestAsk().has_value() == false);  // 105 sibling gone, no asks left
    // The winner's sibling truly gone: a cross at 105 finds nothing.
    eng.submit(limit(4, Side::BUY, 106, 5, 2));
    CHECK(cap.trades() == 1);  // no new trade -- 105 was canceled
  }
  {  // Peg: a bid-pegged buy re-prices to track the best bid as it moves
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::BUY, 99, 5, 1));    // best bid 99
    eng.submit(limit(2, Side::SELL, 101, 5, 2));  // best ask 101
    NewOrder pg = limit(3, Side::BUY, 0, 4, 3);
    pg.peg = PegRef::Bid;  // peg to best bid
    eng.submit(pg);
    // Pegged buy joins the bid at 99 (it is not the top of a new level here).
    CHECK(eng.book().bestBid().value() == px(99));
    // A better bid arrives at 100 -> next submit re-pegs order 3 up to 100.
    eng.submit(limit(4, Side::BUY, 100, 1, 4));   // best bid now 100
    eng.submit(limit(5, Side::SELL, 105, 1, 5));  // a submit boundary to trigger repeg
    CHECK(cap.modifies() >= 1);                   // order 3 was repriced
    // The peg never crosses: with best ask 101, the peg sits at 100, not above.
    CHECK(eng.book().bestBid().value() == px(100));
    // Selling into 100 hits the repriced peg.
    eng.submit(limit(6, Side::SELL, 100, 4, 6));
    CHECK(cap.trades() >= 1);
  }
  {  // Peg self-reference: a Mid peg that BECOMES the touch must not ratchet
     // toward the opposite side on repeat submits -- it must read the market
     // excluding its own resting quantity, so a static market keeps it put.
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::BUY, 100, 5, 1));   // bid 100
    eng.submit(limit(2, Side::SELL, 110, 5, 2));  // ask 110  -> true mid 105
    NewOrder pg = limit(3, Side::BUY, 0, 4, 3);
    pg.peg = PegRef::Mid;
    eng.submit(pg);
    CHECK(eng.book().bestBid().value() == px(105));  // rests at true mid, becomes best bid
    const int modsBefore = cap.modifies();
    // Two unrelated submits that rest away from the touch (best bid/ask unchanged),
    // each triggering a repeg. The market mid never moves -> peg must STAY at 105.
    eng.submit(limit(4, Side::SELL, 115, 1, 4));
    eng.submit(limit(5, Side::SELL, 116, 1, 5));
    CHECK(eng.book().bestBid().value() == px(105));  // no creep (bug: 105->107->108)
    CHECK(cap.modifies() == modsBefore);             // no spurious reprice/priority churn
  }
  {  // Emergency halt-and-cancel: pull the whole resting book + pending stops
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::BUY, 99, 5, 1));
    eng.submit(limit(2, Side::SELL, 101, 5, 2));
    eng.submit(stopMarket(3, Side::BUY, 2, 105, 3));  // pending stop (no last price yet)
    CHECK(!eng.book().empty());
    eng.haltAndCancelAll();
    CHECK(eng.book().empty());                         // resting book pulled
    CHECK(cap.cancels(CancelReason::VenueHalt) == 3);  // 2 limits + 1 stop
    // New orders are rejected while halted.
    eng.submit(limit(4, Side::BUY, 100, 1, 4));
    CHECK(cap.rejects(RejectReason::Halted) == 1);
  }
  {  // Resume after halt through a re-opening auction (not straight to continuous)
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.setHalted(true);
    eng.submit(limit(1, Side::BUY, 100, 5, 1));  // rejected while halted
    CHECK(cap.rejects(RejectReason::Halted) == 1);
    // Operator reopens via auction: orders accumulate without matching.
    eng.resumeWithAuction();
    eng.submit(limit(2, Side::SELL, 100, 5, 2));
    eng.submit(limit(3, Side::BUY, 101, 8, 3));  // crosses, but only accumulates; leaves a remnant
    CHECK(cap.trades() == 0);                    // no matching during the auction
    eng.openContinuous();                        // uncross at the single price
    const int afterAuction = cap.trades();
    CHECK(afterAuction >= 1);    // the re-opening print
    CHECK(!eng.book().empty());  // 3-lot buy remnant rests
    // Continuous now: a fresh cross trades immediately.
    eng.submit(limit(4, Side::SELL, 99, 1, 4));
    CHECK(cap.trades() > afterAuction);
  }
  {  // Firm-group STP: two different accounts of the same firm can't self-trade
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.setStpGroup(10, /*firm*/ 1);
    eng.setStpGroup(11, /*firm*/ 1);                   // accounts 10 and 11 are the same firm
    eng.submit(limit(1, Side::SELL, 100, 5, 10));      // resting, firm 1
    NewOrder cross = limit(2, Side::BUY, 100, 5, 11);  // firm 1, different account
    cross.stp = STPMode::CancelOldest;
    eng.submit(cross);
    CHECK(cap.trades() == 0);  // STP fired across the firm
    CHECK(cap.cancels(CancelReason::SelfTradePrevention) == 1);
    // A third-party account (different firm) trades normally against firm 1.
    eng.submit(limit(3, Side::SELL, 100, 5, 20));  // account 20, no group
    NewOrder ok = limit(4, Side::BUY, 100, 5, 11);
    ok.stp = STPMode::CancelOldest;
    eng.submit(ok);
    CHECK(cap.trades() == 1);  // different firms -> allowed
  }
  {  // A modify re-enters matching as a fresh aggressor, and it must carry the
     // self-trade prevention the order was admitted with. Rebuilding the order
     // from its resting record alone would drop the mode and let the reprice
     // trade against its own account.
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    NewOrder passive = limit(1, Side::BUY, 99, 5, 7);
    passive.stp = STPMode::CancelOldest;
    eng.submit(passive);
    eng.submit(limit(2, Side::SELL, 100, 5, 7));  // same account rests on the other side
    CHECK(cap.trades() == 0);                     // no cross yet
    // Reprice the bid up through the account's own offer.
    eng.submit(InboundCommand{ModifyOrder{1, SYM, px(100), qty(5), 0}});
    CHECK(cap.trades() == 0);  // still no self-trade
    CHECK(cap.cancels(CancelReason::SelfTradePrevention) >= 1);
  }
  {  // account snapshot for reconnect reconciliation: open orders + position
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::BUY, 99, 5, 5));
    eng.submit(limit(2, Side::BUY, 98, 3, 5));
    eng.submit(limit(3, Side::SELL, 101, 2, 6));  // different account
    const auto snap = eng.snapshotAccount(5);
    CHECK(snap.openOrders.size() == 2);
    CHECK(snap.positionQty == 0);  // spot -> flat
    bool has1 = false, has2 = false;
    for (const auto& o : snap.openOrders)
    {
      if (o.id == 1)
      {
        has1 = true;
        CHECK(o.price == px(99) && o.leaves == qty(5) && o.side == Side::BUY);
      }
      if (o.id == 2)
      {
        has2 = true;
        CHECK(o.price == px(98) && o.leaves == qty(3));
      }
    }
    CHECK(has1 && has2);
    CHECK(eng.snapshotAccount(6).openOrders.size() == 1);  // account 6 sees only its own
    CHECK(eng.snapshotAccount(999).openOrders.empty());    // unknown account -> empty
    // A pending stop (in the stop book, not the visible book) shows up too.
    eng.submit(stopMarket(7, Side::BUY, 4, 105, 5));  // acct 5 pending stop, trigger 105
    const auto snap2 = eng.snapshotAccount(5);
    CHECK(snap2.openOrders.size() == 2);    // limits unchanged
    CHECK(snap2.pendingStops.size() == 1);  // the stop is surfaced
    CHECK(snap2.pendingStops[0].id == 7 && snap2.pendingStops[0].trigger == px(105));
  }
  {  // live risk-limit adjustment: tighten max-open-orders at runtime
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::BUY, 99, 1, 5));
    eng.submit(limit(2, Side::BUY, 98, 1, 5));  // both rest (no cap yet)
    CHECK(cap.accepts() == 2);
    eng.setMaxOpenOrders(2);                    // operator tightens the cap live
    eng.submit(limit(3, Side::BUY, 97, 1, 5));  // account 5 already at 2 -> rejected
    CHECK(cap.rejects(RejectReason::TooManyOpenOrders) == 1);
  }
  {  // Deterministic cancel order: mass-cancel emits OrderCanceled in ascending
     // id order regardless of insertion order -- the emitted event stream must not
     // depend on unordered-container layout (HA/replay equivalence across builds).
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(30, Side::BUY, 95, 1, 5));
    eng.submit(limit(10, Side::BUY, 94, 1, 5));
    eng.submit(limit(20, Side::BUY, 93, 1, 5));  // scrambled ids, same account
    eng.submit(MassCancel{5, SYM});
    std::vector<OrderId> canceled;
    for (const auto& e : cap.ev)
    {
      if (const auto* c = std::get_if<OrderCanceled>(&e))
      {
        canceled.push_back(c->id);
      }
    }
    CHECK(canceled.size() == 3);
    CHECK(canceled.size() == 3 && canceled[0] == 10 && canceled[1] == 20 && canceled[2] == 30);
  }
  {  // OCO cleanup: a canceled leg must be unlinked from ocoMembers_ so a REUSED
     // id is not wrongly canceled when the surviving sibling later resolves.
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    NewOrder a = limit(10, Side::SELL, 102, 1, 1);
    a.ocoGroup = 1;
    NewOrder b = limit(11, Side::SELL, 105, 1, 1);
    b.ocoGroup = 1;
    eng.submit(a);
    eng.submit(b);                               // group 1 = [10, 11], both resting
    eng.submit(CancelOrder{10, SYM, 1});         // cancel leg A -> must unlink 10 from group 1
    eng.submit(limit(10, Side::BUY, 95, 1, 2));  // REUSE id 10: unrelated buy, no OCO
    CHECK(eng.book().bestBid().has_value() && eng.book().bestBid().value() == px(95));
    eng.submit(limit(12, Side::BUY, 105, 1, 3));  // lift leg B -> resolves group 1
    CHECK(cap.trades() == 1);
    // The reused id-10 buy must be untouched (a stale ocoMembers_[1] would cancel it).
    CHECK(eng.book().bestBid().has_value() && eng.book().bestBid().value() == px(95));
    CHECK(eng.snapshotAccount(2).openOrders.size() == 1);
  }
  {  // OCO cleanup on REJECT: a rejected OCO leg must be unlinked (scope guard),
     // so a reused id is not wrongly canceled when the sibling resolves.
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    NewOrder a = limit(20, Side::SELL, 105, 1, 1);
    a.ocoGroup = 2;
    eng.submit(a);  // group 2 = [20], resting
    NewOrder bad = limit(21, Side::SELL, 200, 1, 1);
    bad.ocoGroup = 2;  // price > maxPrice(119)
    eng.submit(bad);   // rejected by validate AFTER the OCO link
    CHECK(cap.rejects(RejectReason::InvalidPrice) >= 1);
    eng.submit(limit(21, Side::BUY, 95, 1, 2));                                         // REUSE the rejected id 21
    eng.submit(limit(22, Side::BUY, 105, 1, 3));                                        // lift leg A -> resolves group 2
    CHECK(eng.book().bestBid().has_value() && eng.book().bestBid().value() == px(95));  // reused id alive
    CHECK(eng.snapshotAccount(2).openOrders.size() == 1);
  }
  {  // FOK + STP: self-liquidity must NOT let a FOK pass the precheck and rest.
    // The only crossing liquidity is the account's own ask, which STP would
    // cancel (never trade), so the FOK cannot fully fill -> it must be killed,
    // all-or-none, and never become a resting GTC bid.
    Capture cap;
    MatchingEngine<Book> eng(cfg(), cap.sink(), mk());
    eng.submit(limit(1, Side::SELL, 100, 5, 7));                      // account 7 rests SELL 5 @ 100
    NewOrder fok = limit(2, Side::BUY, 100, 5, 7, TimeInForce::FOK);  // same account
    fok.stp = STPMode::CancelOldest;
    eng.submit(fok);
    CHECK(cap.trades() == 0);
    CHECK(cap.rejects(RejectReason::FillOrKillUnfulfillable) == 1);                      // rejected pre-match
    CHECK(!eng.book().bestBid().has_value());                                            // the FOK did NOT rest as a bid
    CHECK(eng.book().bestAsk().has_value() && eng.book().bestAsk().value() == px(100));  // ask intact
    CHECK(eng.snapshotAccount(7).openOrders.size() == 1);                                // only the original resting sell
  }
}

}  // namespace

TEST(Matcher, EngineSuite)
{
  runAll<MatchingBook>([]
                       { return MatchingBook{}; }, "map-reference");
  runAll<LadderBook>([]
                     { return LadderBook{ladderCfg()}; }, "ladder-o1");

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
