/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr SymbolId SYM = 1;
constexpr AssetId QUOTE = 1;  // collateral (USD)
constexpr uint64_t VENUE = 999;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;  // 10% -> 10x
  return c;
}
NewOrder ord(OrderId id, Side s, double p, double q, uint64_t acct, bool reduceOnly = false)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  o.reduceOnly = reduceOnly;
  return o;
}
NewOrder stopMkt(OrderId id, Side s, double q, double trig, uint64_t acct, bool reduceOnly = false)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::STOP_MARKET;
  o.quantity = qty(q);
  o.triggerPrice = px(trig);
  o.accountId = acct;
  o.reduceOnly = reduceOnly;
  return o;
}

void test_open_and_close()
{
  std::printf("test_perp_open_close\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(1000));
  led.deposit(2, QUOTE, quote(1000));
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);   // IM 100
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);  // IM 100, trade 10@100

  CHECK(eng.positionQty(1) == qty(10).raw());   // long 10
  CHECK(eng.positionQty(2) == -qty(10).raw());  // short 10
  CHECK(eng.positionEntry(1) == px(100));
  CHECK(led.available(1, QUOTE) == quote(900) && led.reserved(1, QUOTE) == quote(100));

  // Close both at 110: long +100 PnL, short -100 PnL, funded via the pool.
  eng.submit(InboundCommand{ord(3, Side::SELL, 110, 10, 1, /*reduceOnly*/ true)}, 2);
  eng.submit(InboundCommand{ord(4, Side::BUY, 110, 10, 2, /*reduceOnly*/ true)}, 3);

  CHECK(eng.positionQty(1) == 0 && eng.positionQty(2) == 0);
  CHECK(led.available(1, QUOTE) == quote(1100));  // +100 profit, margin returned
  CHECK(led.available(2, QUOTE) == quote(900));   // -100 loss
  CHECK(led.reserved(1, QUOTE) == 0 && led.reserved(2, QUOTE) == 0);
  // Conservation across traders + clearing pool.
  const Amount tot = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(tot == quote(2000));
}

void test_insufficient_margin()
{
  std::printf("test_perp_insufficient_margin\n");
  Ledger led;
  led.deposit(3, QUOTE, quote(50));  // needs IM 100 for 10@100
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 3)}, 0);
  bool rejected = false;
  for (auto& e : ev)
  {
    if (auto* r = std::get_if<OrderRejected>(&e); r && r->reason == RejectReason::InsufficientFunds)
    {
      rejected = true;
    }
  }
  CHECK(rejected);
}

void test_funding()
{
  std::printf("test_perp_funding\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(1000));
  led.deposit(2, QUOTE, quote(1000));
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);

  eng.applyFunding(/*rate*/ 0.01, /*mark*/ px(100));  // notional 1000 -> 10 pays
  CHECK(led.available(1, QUOTE) == quote(890));       // long paid 10 (900 - 10)
  CHECK(led.available(2, QUOTE) == quote(910));       // short received 10
  CHECK(led.total(VENUE, QUOTE) == 0);                // pool net zero
}

bool liquidated(const std::vector<OutboundEvent>& ev, uint64_t acct, bool& bankrupt)
{
  for (auto& e : ev)
  {
    if (auto* l = std::get_if<Liquidation>(&e); l && l->account == acct)
    {
      bankrupt = l->bankrupt;
      return true;
    }
  }
  return false;
}

void test_liquidation()
{
  std::printf("test_perp_liquidation\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(100));  // exactly the IM for 10@100 -> highly leveraged
  led.deposit(2, QUOTE, quote(1000));
  auto c = cfg();
  c.maintenanceMarginBps = 500;  // 5%
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);
  CHECK(eng.positionQty(1) == qty(10).raw());
  CHECK(led.available(1, QUOTE) == 0 && led.reserved(1, QUOTE) == quote(100));

  // Mark drops to 91: equity = 100 + (91-100)*10 = 10 < MM(910*5%=45.5) -> liquidate acct1.
  eng.setMarkPrice(px(91));
  bool bankrupt = true;
  CHECK(liquidated(ev, 1, bankrupt));
  CHECK(!bankrupt);
  CHECK(eng.positionQty(1) == 0);
  CHECK(led.available(1, QUOTE) == quote(10));  // equity returned
  CHECK(eng.positionQty(2) == -qty(10).raw());  // short survives (profit)

  // Conservation.
  const Amount tot = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(tot == quote(1100));
}

void test_bankruptcy()
{
  std::printf("test_perp_bankruptcy\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(100));
  led.deposit(2, QUOTE, quote(1000));
  auto c = cfg();
  c.maintenanceMarginBps = 500;
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);

  // Mark crashes to 85: equity = 100 - 150 = -50 -> bankrupt, insurance covers.
  eng.setMarkPrice(px(85));
  bool bankrupt = false;
  CHECK(liquidated(ev, 1, bankrupt));
  CHECK(bankrupt);
  CHECK(led.available(1, QUOTE) == 0);  // wiped out, insurance topped up to zero
  CHECK(eng.positionQty(1) == 0);
  // Pool holds the long's booked loss (backs the short's gain); nets to the -50
  // insurance deficit once the short also settles. Value is conserved throughout.
  const Amount tot = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(tot == quote(1100));
}

// A liquidated account's OTHER resting orders lock initial margin in `reserved`
// -- the account's own collateral. Liquidation must cancel them first so that
// collateral covers the position's shortfall, instead of the insurance fund
// paying out to an account that is solvent on a total-equity basis.
void test_liquidation_frees_resting_collateral()
{
  std::printf("test_perp_liquidation_frees_resting_collateral\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(200));   // funds IM 100 (position) + IM 100 (resting)
  led.deposit(2, QUOTE, quote(1000));  // counterparty
  auto c = cfg();
  c.maintenanceMarginBps = 500;
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);

  // Acct1 opens long 10 @ 100 (IM 100), then rests a second buy 10 @ 100 that
  // does not fill (empty book) -- reserving another IM 100. avail 0, reserved 200.
  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);
  eng.submit(InboundCommand{ord(3, Side::BUY, 100, 10, 1)}, 2);  // rests, reserves IM 100
  CHECK(led.available(1, QUOTE) == 0);
  CHECK(led.reserved(1, QUOTE) == quote(200));

  // Mark crashes to 85: position equity = 100 - 150 = -50. But acct1 holds 100 of
  // its own collateral behind the resting order. After that order is canceled and
  // its IM freed, the account covers its own 50 shortfall -> NOT bankrupt, no
  // insurance payout. Deposited 200, lost 150 on the position -> ends with 50.
  eng.setMarkPrice(px(85));
  bool bankrupt = true;
  CHECK(liquidated(ev, 1, bankrupt));
  CHECK(!bankrupt);  // solvent on total equity -> no insurance
  CHECK(eng.positionQty(1) == 0);
  CHECK(led.total(1, QUOTE) == quote(50));  // buggy path leaves 100 (a 50 windfall)
  bool restingCanceled = false;
  for (auto& e : ev)
  {
    if (auto* c2 = std::get_if<OrderCanceled>(&e);
        c2 && c2->id == 3 && c2->reason == CancelReason::Liquidation)
    {
      restingCanceled = true;
    }
  }
  CHECK(restingCanceled);  // its collateral was freed, not stranded
  // Conservation across accounts + venue.
  const Amount tot = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(tot == quote(1200));
}

// Funding is charged to the wallet (`available`). A max-leverage payer with no
// free collateral goes wallet-negative on funding; that negative must drag the
// maintenance check so an unaffordable funding payment triggers liquidation
// instead of accruing unbounded silent bad debt.
void test_funding_triggers_liquidation()
{
  std::printf("test_perp_funding_triggers_liquidation\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(100));     // exactly IM for long 10 @ 100 -> avail 0
  led.deposit(2, QUOTE, quote(100000));  // deep, healthy counterparty
  auto c = cfg();
  c.maintenanceMarginBps = 500;  // mmReq at mark 100 = 1000 * 5% = 50
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);
  CHECK(led.available(1, QUOTE) == 0);  // max leverage: no free collateral

  // Mark unchanged (uPnl 0), but a 6% funding charge pays 60 from a wallet that
  // has 0 free -> avail -60. Equity = margin 100 + uPnl 0 + walletDrag(-60) = 40
  // < mmReq 50 -> liquidation. Without the wallet-drag term the check sees only
  // margin+uPnl = 100 and the account keeps its position with a -60 wallet.
  eng.applyFunding(/*rate*/ 0.06, /*mark*/ px(100));
  bool bankrupt = true;
  CHECK(liquidated(ev, 1, bankrupt));
  CHECK(!bankrupt);  // margin covers the funding shortfall
  CHECK(eng.positionQty(1) == 0);
  CHECK(led.total(1, QUOTE) == quote(40));  // 100 deposited - 60 funding paid
  // Conservation across accounts + venue (funding is a long->short transfer).
  const Amount tot = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(tot == quote(100100));
}

// A reduce-only conditional (stop) order that fires from a FLAT account must NOT
// open a position: it re-enters matching via processTriggers, not onNew, so the
// reduce-only cap must be applied there too -- otherwise it opens an
// uncollateralized (margin=0) position, violating "reduce-only never opens".
void test_reduce_only_stop_cannot_open()
{
  std::printf("test_perp_reduce_only_stop_cannot_open\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(1000));    // flat account -- owns the reduce-only stop
  led.deposit(2, QUOTE, quote(100000));  // resting bid liquidity
  led.deposit(3, QUOTE, quote(100000));  // triggers the print
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{ord(1, Side::BUY, 95, 10, 2)}, 0);                            // acct2 bid 10 @ 95
  eng.submit(InboundCommand{stopMkt(10, Side::SELL, 5, 95, 1, /*reduceOnly*/ true)}, 1);  // parked
  CHECK(eng.positionQty(1) == 0);
  // A trade at 95 sets last = 95 -> the SELL stop (trigger 95) fires from FLAT.
  eng.submit(InboundCommand{ord(2, Side::SELL, 95, 3, 3)}, 2);
  CHECK(eng.positionQty(1) == 0);  // must stay flat -- bug opened an im=0 short of -5
  bool rejected = false;
  for (auto& e : ev)
  {
    if (auto* r = std::get_if<OrderRejected>(&e);
        r && r->id == 10 && r->reason == RejectReason::InvalidQuantity)
    {
      rejected = true;
    }
  }
  CHECK(rejected);  // reduce-only-from-flat capped to 0 -> rejected, not opened
}

// A modify (cancel/replace) of a perp order re-enters matching outside onNew, so
// it must still honor maxPositionQty -- else a size-increase modify grows the
// position past the cap that a fresh order of that size would be rejected for.
void test_perp_modify_respects_position_cap()
{
  std::printf("test_perp_modify_respects_position_cap\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(100000));
  auto c = cfg();
  c.maxPositionQty = qty(10);
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);  // rests at the cap (no ask -> no fill)
  CHECK(eng.book().bestBid().has_value() && eng.book().bestBid().value() == px(100));
  ev.clear();
  // Grow it to 20 (> cap). A fresh qty-20 order is rejected in onNew; the modify
  // must be too, not silently accepted.
  eng.submit(InboundCommand{ModifyOrder{1, SYM, px(100), qty(20), 1}}, 1);
  bool rejected = false;
  for (auto& e : ev)
  {
    if (auto* r = std::get_if<OrderRejected>(&e);
        r && r->id == 1 && r->reason == RejectReason::PositionLimitExceeded)
    {
      rejected = true;
    }
  }
  CHECK(rejected);
  CHECK(!eng.book().bestBid().has_value());  // order gone, not resting at qty 20
}

// The perp risk gate (reduce-only cap + position-limit) is one function shared
// by the new-order, triggered-stop, and modify paths. This pins that they agree:
// the identical over-cap scenario must reject with PositionLimitExceeded whether
// it arrives as a fresh order or as a size-increasing modify.
void test_perp_risk_gate_consistent_across_paths()
{
  std::printf("test_perp_risk_gate_consistent_across_paths\n");
  auto rejectsOverCap = [](bool viaModify)
  {
    Ledger led;
    led.deposit(1, QUOTE, quote(100000));
    auto c = cfg();
    c.maxPositionQty = qty(10);
    std::vector<OutboundEvent> ev;
    MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                     { ev.push_back(e); });
    eng.setLedger(&led, VENUE);
    eng.submit(InboundCommand{ord(1, Side::BUY, 100, 8, 1)}, 0);  // rest 8 (<= cap)
    ev.clear();
    if (viaModify)
    {
      eng.submit(InboundCommand{ModifyOrder{1, SYM, px(100), qty(20), 1}}, 1);  // 8 -> 20
    }
    else
    {
      eng.submit(InboundCommand{ord(2, Side::BUY, 100, 20, 1)}, 1);  // fresh 20
    }
    for (auto& e : ev)
    {
      if (auto* r = std::get_if<OrderRejected>(&e);
          r && r->reason == RejectReason::PositionLimitExceeded)
      {
        return true;
      }
    }
    return false;
  };
  CHECK(rejectsOverCap(/*viaModify*/ false));  // new-order path
  CHECK(rejectsOverCap(/*viaModify*/ true));   // modify path
}

// reduce-only must survive a modify: a resting reduce-only order that is
// repriced/resized re-enters matching outside onNew, and its reduce-only nature
// (carried on the resting record) must be preserved and re-capped -- else the
// modified order can flip the position, the exact thing reduce-only forbids.
void test_perp_modify_preserves_reduce_only()
{
  std::printf("test_perp_modify_preserves_reduce_only\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(100000));
  led.deposit(2, QUOTE, quote(100000));
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 5, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 5, 2)}, 1);  // acct1 long +5
  CHECK(eng.positionQty(1) == qty(5).raw());
  // Reduce-only SELL rests above market (capped to the +5 position at submit).
  eng.submit(InboundCommand{ord(3, Side::SELL, 110, 5, 1, /*reduceOnly*/ true)}, 2);
  // Modify to a LARGER qty (10): reduce-only must be preserved and re-capped to 5.
  eng.submit(InboundCommand{ModifyOrder{3, SYM, px(110), qty(10), 1}}, 3);
  // Fill it fully: acct2 buys 10 @ 110. Only 5 should rest -> acct1 reduces to FLAT.
  eng.submit(InboundCommand{ord(4, Side::BUY, 110, 10, 2)}, 4);
  CHECK(eng.positionQty(1) == 0);  // reduced to flat -- bug flips it to -5 (opened a short)
}

// T029: an unpriced (market / stop) perp order is bounded by the price band for
// margin purposes, and on a linear perp that bound is the band's TOP on BOTH
// sides -- nothing is delivered, so notional and initial margin grow with price
// whether the account ends long or short. Bounding a SELL at minPrice (correct
// only for a SPOT seller) under-reserves it by the whole band ratio and
// consumeOrderIM then posts the position that under-reserved margin.
void test_perp_unpriced_sell_margin_matches_buy()
{
  std::printf("test_perp_unpriced_sell_margin_matches_buy\n");
  // Band is [1, 1000] and IM is 10%: an unpriced order for 10 contracts must
  // reserve 10 * 1000 * 10% = 1000 quote regardless of side (bounding a sell at
  // minPrice would have asked for 1).
  const Amount imAtBand = quote(1000);

  auto marginAfterMarketFill = [&](Side takerSide)
  {
    Ledger led;
    led.deposit(1, QUOTE, quote(100000));  // resting counterparty
    led.deposit(2, QUOTE, imAtBand);       // aggressor: exactly the band-bounded IM
    MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
    eng.setLedger(&led, VENUE);
    const Side restingSide = (takerSide == Side::BUY) ? Side::SELL : Side::BUY;
    eng.submit(InboundCommand{ord(1, restingSide, 100, 10, 1)}, 0);
    NewOrder mkt;
    mkt.id = 2;
    mkt.symbol = SYM;
    mkt.side = takerSide;
    mkt.type = OrderType::MARKET;
    mkt.quantity = qty(10);
    mkt.accountId = 2;
    eng.submit(InboundCommand{mkt}, 1);
    CHECK(eng.positionQty(2) == (takerSide == Side::BUY ? qty(10).raw() : -qty(10).raw()));
    return led.reserved(2, QUOTE);  // == the position's posted margin
  };

  const Amount buyMargin = marginAfterMarketFill(Side::BUY);
  const Amount sellMargin = marginAfterMarketFill(Side::SELL);
  CHECK(buyMargin == imAtBand);
  CHECK(sellMargin == buyMargin);  // the short was margined at minPrice before the fix

  // And the reservation gate is symmetric: one raw unit short of the
  // band-bounded IM must reject either side, not just the buy.
  auto rejectsAtDeposit = [](Side side, Amount deposit)
  {
    Ledger led;
    led.deposit(2, QUOTE, deposit);
    std::vector<OutboundEvent> ev;
    MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                     { ev.push_back(e); });
    eng.setLedger(&led, VENUE);
    NewOrder mkt;
    mkt.id = 1;
    mkt.symbol = SYM;
    mkt.side = side;
    mkt.type = OrderType::MARKET;
    mkt.quantity = qty(10);
    mkt.accountId = 2;
    eng.submit(InboundCommand{mkt}, 0);
    for (auto& e : ev)
    {
      if (auto* r = std::get_if<OrderRejected>(&e);
          r && r->reason == RejectReason::InsufficientFunds)
      {
        return true;
      }
    }
    return false;
  };
  CHECK(rejectsAtDeposit(Side::SELL, imAtBand - 1));
  CHECK(rejectsAtDeposit(Side::BUY, imAtBand - 1));
  CHECK(!rejectsAtDeposit(Side::SELL, imAtBand));
  CHECK(!rejectsAtDeposit(Side::BUY, imAtBand));
}

// T030: reduce-only and maxPositionQty are gated when an order is ADMITTED, but
// a resting order fills later -- against a position that has moved since. A
// reduce-only order reserves no initial margin, so any part of it that opens or
// flips the position opens it with ZERO margin; the flags must therefore be
// re-measured at fill time, on the live position.
void test_reduce_only_resting_cannot_flip_at_fill()
{
  std::printf("test_perp_reduce_only_resting_cannot_flip_at_fill\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(100000));
  led.deposit(2, QUOTE, quote(100000));
  led.deposit(3, QUOTE, quote(100000));
  const Amount init = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(3, QUOTE) +
                      led.total(VENUE, QUOTE);
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);
  CHECK(eng.positionQty(1) == qty(10).raw());  // acct1 long 10

  // Reduce-only exit for the WHOLE position, resting above the market.
  eng.submit(InboundCommand{ord(3, Side::SELL, 110, 10, 1, /*reduceOnly*/ true)}, 2);
  // The position then shrinks by 6 through another exit, leaving +4 behind the
  // resting order's 10.
  eng.submit(InboundCommand{ord(4, Side::SELL, 100, 6, 1, /*reduceOnly*/ true)}, 3);
  eng.submit(InboundCommand{ord(5, Side::BUY, 100, 6, 3)}, 4);
  CHECK(eng.positionQty(1) == qty(4).raw());

  // Someone lifts the whole resting reduce-only order: only the 4 that still
  // reduces may print. The rest must not flip acct1 into a short it posted no
  // margin for.
  ev.clear();
  eng.submit(InboundCommand{ord(6, Side::BUY, 110, 10, 2)}, 5);
  CHECK(eng.positionQty(1) == 0);  // flat, never short
  CHECK(eng.totalPositionMargin() >= 0);
  bool canceled = false;
  for (auto& e : ev)
  {
    if (auto* c = std::get_if<OrderCanceled>(&e);
        c && c->id == 3 && c->reason == CancelReason::ReduceOnlyNotReducing)
    {
      canceled = true;
    }
  }
  CHECK(canceled);  // the residual that could not reduce is pulled, with a reason
  CHECK(!eng.book().contains(3));

  // Margin invariant and conservation both hold afterwards (drained first, so
  // every reserved unit left must be a position's posted margin).
  for (OrderId id = 1; id <= 6; ++id)
  {
    eng.submit(InboundCommand{CancelOrder{id, SYM, 0}}, 6);
  }
  Amount reservedSum = 0;
  for (uint64_t a = 1; a <= 3; ++a)
  {
    reservedSum += led.reserved(a, QUOTE);
  }
  CHECK(reservedSum == eng.totalPositionMargin());
  CHECK(led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(3, QUOTE) + led.total(VENUE, QUOTE) ==
        init);
}

// The auction uncross prints its own fills instead of going through the
// matcher, so it needs the same fill-time re-check: a reduce-only order that
// rested through a position change must not be flipped by the reopening
// auction either.
void test_auction_uncross_respects_reduce_only()
{
  std::printf("test_perp_auction_uncross_respects_reduce_only\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(100000));
  led.deposit(2, QUOTE, quote(100000));
  led.deposit(3, QUOTE, quote(100000));
  const Amount init = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(3, QUOTE) +
                      led.total(VENUE, QUOTE);
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);  // acct1 long 10
  eng.submit(InboundCommand{ord(3, Side::SELL, 110, 10, 1, /*reduceOnly*/ true)}, 2);
  eng.submit(InboundCommand{ord(4, Side::SELL, 100, 6, 1, /*reduceOnly*/ true)}, 3);
  eng.submit(InboundCommand{ord(5, Side::BUY, 100, 6, 3)}, 4);
  CHECK(eng.positionQty(1) == qty(4).raw());  // order 3 now over-sized for the position

  // Reopening auction: accumulate a bid that lifts the whole resting sell.
  eng.beginPreOpen();
  eng.submit(InboundCommand{ord(6, Side::BUY, 110, 10, 2)}, 5);
  ev.clear();
  eng.openContinuous();

  CHECK(eng.positionQty(1) == 0);  // reduced to flat, never flipped short
  bool canceled = false;
  for (auto& e : ev)
  {
    if (auto* c = std::get_if<OrderCanceled>(&e);
        c && c->id == 3 && c->reason == CancelReason::ReduceOnlyNotReducing)
    {
      canceled = true;
    }
  }
  CHECK(canceled);
  CHECK(led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(3, QUOTE) + led.total(VENUE, QUOTE) ==
        init);
}

// T030: maxPositionQty is checked against the INCOMING order at admission, so
// two orders that each pass individually can settle into a position past the
// cap. The cap must bind the RESULTING position, at fill time.
void test_position_cap_not_circumvented_by_several_orders()
{
  std::printf("test_perp_position_cap_not_circumvented_by_several_orders\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(100000));
  led.deposit(2, QUOTE, quote(100000));
  led.deposit(3, QUOTE, quote(100000));
  auto c = cfg();
  c.maxPositionQty = qty(10);
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);

  // Two resting bids of 6 each: each passes the admission cap (the position is
  // 0 when both are placed), together they would build 12 against a cap of 10.
  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 6, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::BUY, 100, 6, 1)}, 1);
  eng.submit(InboundCommand{ord(3, Side::SELL, 100, 6, 2)}, 2);  // fills the first: +6
  CHECK(eng.positionQty(1) == qty(6).raw());
  ev.clear();
  eng.submit(InboundCommand{ord(4, Side::SELL, 100, 6, 3)}, 3);  // would take acct1 to +12

  CHECK(eng.positionQty(1) == qty(10).raw());  // capped exactly, not 12
  bool capCanceled = false;
  for (auto& e : ev)
  {
    if (auto* cc = std::get_if<OrderCanceled>(&e);
        cc && cc->id == 2 && cc->reason == CancelReason::PositionLimitExceeded)
    {
      capCanceled = true;
    }
  }
  CHECK(capCanceled);  // the maker slice that would breach the cap is pulled
  CHECK(!eng.book().contains(2));

  for (OrderId id = 1; id <= 4; ++id)
  {
    eng.submit(InboundCommand{CancelOrder{id, SYM, 0}}, 4);
  }
  Amount reservedSum = led.reserved(1, QUOTE) + led.reserved(2, QUOTE) + led.reserved(3, QUOTE);
  CHECK(reservedSum == eng.totalPositionMargin());
}

void test_engine_adl()
{
  std::printf("test_perp_engine_adl\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(100));     // exactly IM for 10@100 -> max leverage
  led.deposit(2, QUOTE, quote(100000));  // deep winner
  auto c = cfg();
  c.maintenanceMarginBps = 500;
  c.autoDeleverage = true;
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);
  const Amount init = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);

  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);
  const Amount venueBefore = led.total(VENUE, QUOTE);  // insurance level

  // Mark crashes to 85: acct1 equity = 100 + (85-100)*10 = -50 (bankrupt, deficit 50).
  eng.setMarkPrice(px(85));
  CHECK(eng.positionQty(1) == 0);
  CHECK(eng.positionQty(2) == 0);  // winner auto-deleveraged

  // Insurance spared: the 50 deficit was clawed from acct2's forgone profit.
  CHECK(led.total(VENUE, QUOTE) == venueBefore);

  bool sawAdl = false, sawBankrupt = false;
  for (const auto& e : ev)
  {
    if (const auto* l = std::get_if<Liquidation>(&e))
    {
      if (l->adl)
      {
        sawAdl = true;
      }
      if (l->bankrupt)
      {
        sawBankrupt = true;
      }
    }
  }
  CHECK(sawAdl);
  CHECK(sawBankrupt);
  CHECK(led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE) == init);
}

void test_position_limit()
{
  std::printf("test_perp_position_limit\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(100000));
  led.deposit(2, QUOTE, quote(100000));
  auto c = cfg();
  c.maxPositionQty = qty(10);  // cap |position| at 10 contracts
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);

  // Open exactly at the cap: allowed.
  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);
  CHECK(eng.positionQty(1) == qty(10).raw());

  // One more contract in the same direction would breach the cap -> rejected.
  ev.clear();
  eng.submit(InboundCommand{ord(3, Side::BUY, 100, 1, 1)}, 2);
  bool rejected = false;
  for (auto& e : ev)
  {
    if (auto* r = std::get_if<OrderRejected>(&e);
        r && r->reason == RejectReason::PositionLimitExceeded)
    {
      rejected = true;
    }
  }
  CHECK(rejected);
  CHECK(eng.positionQty(1) == qty(10).raw());  // unchanged

  // A reduce-only order in the opposite direction is still allowed.
  ev.clear();
  eng.submit(InboundCommand{ord(4, Side::SELL, 100, 5, 1, /*reduceOnly*/ true)}, 3);
  eng.submit(InboundCommand{ord(5, Side::BUY, 100, 5, 2)}, 4);
  CHECK(eng.positionQty(1) == qty(5).raw());  // reduced to 5
}

}  // namespace

// ---- T031: a ledgerless perp still tracks exposure --------------------------
//
// Positions are exposure, not cash. With no ledger bound the engine skips
// settlement, and it used to skip position tracking with it -- which made every
// reduce-only order reject ("nothing to reduce"), degraded maxPositionQty to a
// per-order cap, and published open interest as a flat zero on the public feed.

void test_ledgerless_perp_tracks_exposure()
{
  std::printf("test_ledgerless_perp_tracks_exposure\n");
  SymbolConfig c = cfg();
  c.maxPositionQty = qty(10);
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  // no setLedger: funds live in another system

  // Open a 5-lot long for account 1 against account 2.
  eng.submit(InboundCommand{ord(1, Side::SELL, 100, 5, 2)}, 1);
  eng.submit(InboundCommand{ord(2, Side::BUY, 100, 5, 1)}, 2);
  CHECK(eng.positionQty(1) == qty(5).raw());
  CHECK(eng.positionQty(2) == -qty(5).raw());
  CHECK(eng.openInterest() == qty(5));  // was a flat zero regardless of volume

  // Reduce-only now has something to reduce and is accepted.
  eng.submit(InboundCommand{ord(3, Side::BUY, 100, 3, 2)}, 3);
  eng.submit(InboundCommand{ord(4, Side::SELL, 100, 3, 1, /*reduceOnly=*/true)}, 4);
  bool bogusReject = false;
  for (auto& e : ev)
  {
    if (auto* r = std::get_if<OrderRejected>(&e);
        r != nullptr && r->reason == RejectReason::InvalidQuantity)
    {
      bogusReject = true;
    }
  }
  CHECK(!bogusReject);  // used to reject every reduce-only: "nothing to reduce"
  CHECK(eng.positionQty(1) == qty(2).raw());
  CHECK(eng.openInterest() == qty(2));

  // The position cap is cumulative again, not per order: 6 + 6 > 10.
  // Two 6-lot buys against separate makers: each is under the 10 cap on its
  // own, so only a cumulative check can stop the pair. Makers are distinct
  // accounts because the cap binds their short side too.
  std::vector<OutboundEvent> ev2;
  MatchingEngine<MatchingBook> eng2(c, [&](const OutboundEvent& e)
                                    { ev2.push_back(e); });
  eng2.submit(InboundCommand{ord(10, Side::SELL, 100, 6, 2)}, 10);
  eng2.submit(InboundCommand{ord(11, Side::BUY, 100, 6, 1)}, 11);
  CHECK(eng2.positionQty(1) == qty(6).raw());
  eng2.submit(InboundCommand{ord(12, Side::SELL, 100, 6, 3)}, 12);
  eng2.submit(InboundCommand{ord(13, Side::BUY, 100, 6, 1)}, 13);
  // 6 + 6 would be 12 against a cap of 10, so the second order is refused
  // outright rather than trimmed. Without a cumulative check the position
  // would have reached 12: each order is under the cap on its own.
  CHECK(eng2.positionQty(1) == qty(6).raw());

  // No money moved anywhere: settlement is the other system's job.
  CHECK(eng.ledger() == nullptr);
}

// A money command against a ledgerless engine is answered, not swallowed.
void test_ledgerless_money_commands_answer()
{
  std::printf("test_ledgerless_money_commands_answer\n");
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  Deposit d{};
  d.accountId = 7;
  d.asset = QUOTE;
  d.amountRaw = 1000;
  eng.submit(InboundCommand{d}, 1);
  bool depositAnswered = false;
  for (auto& e : ev)
  {
    if (auto* r = std::get_if<OrderRejected>(&e);
        r != nullptr && r->reason == RejectReason::NoLedgerBound)
    {
      depositAnswered = true;
    }
  }
  CHECK(depositAnswered);  // used to vanish with no event at all

  std::vector<OutboundEvent> ev2;
  MatchingEngine<MatchingBook> eng2(cfg(), [&](const OutboundEvent& e)
                                    { ev2.push_back(e); });
  Withdraw w{};
  w.accountId = 7;
  w.asset = QUOTE;
  w.amountRaw = 1000;
  eng2.submit(InboundCommand{w}, 1);
  bool withdrawAnswered = false;
  for (auto& e : ev2)
  {
    if (auto* r = std::get_if<OrderRejected>(&e);
        r != nullptr && r->reason == RejectReason::NoLedgerBound)
    {
      withdrawAnswered = true;
    }
  }
  CHECK(withdrawAnswered);
}

// ---- external risk owner: the three seams it needs ------------------------
//
// A portfolio-margin model lives above the per-symbol engines: it sees the
// whole basket, so it decides entry and liquidation, and the engine executes.
// This exercises all three seams together the way such an owner would use
// them: approve/refuse on entry, watch fills, close on its own decision.

void test_external_risk_owner_seams()
{
  std::printf("test_external_risk_owner_seams\n");
  using Eng = MatchingEngine<MatchingBook>;
  Ledger led;
  led.deposit(1, QUOTE, quote(10000));
  led.deposit(2, QUOTE, quote(10000));
  std::vector<OutboundEvent> ev;
  SymbolConfig c = cfg();
  c.maintenanceMarginBps = 500;
  c.externalLiquidation = true;  // the engine must not liquidate on its own
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);

  // Seam 1: the risk owner approves entry and can say why it refuses.
  bool allow = true;
  eng.setCreditCheck(
      [&](const Eng::CreditRequest& r) -> Eng::CreditDecision
      {
        CHECK(r.symbol == SYM);  // it serves many instruments: it must be told which
        if (!allow && !r.reduceOnly)
        {
          return {false, RejectReason::PositionLimitExceeded};
        }
        return {true, RejectReason::None};
      });

  eng.submit(InboundCommand{ord(1, Side::SELL, 100, 5, 2)}, 1);
  eng.submit(InboundCommand{ord(2, Side::BUY, 100, 5, 1)}, 2);
  CHECK(eng.positionQty(1) == qty(5).raw());

  // Refusal carries the owner's reason to the client, not a flat funds error.
  allow = false;
  ev.clear();
  eng.submit(InboundCommand{ord(3, Side::BUY, 100, 1, 1)}, 3);
  bool refusedWithReason = false;
  for (auto& e : ev)
  {
    if (auto* r = std::get_if<OrderRejected>(&e);
        r != nullptr && r->reason == RejectReason::PositionLimitExceeded)
    {
      refusedWithReason = true;
    }
  }
  CHECK(refusedWithReason);
  allow = true;

  // Seam 2: the engine does NOT liquidate by itself, however bad the mark.
  ev.clear();
  eng.setMarkPrice(px(10));  // far past maintenance for a 5-lot long at 100
  bool selfLiquidated = false;
  for (auto& e : ev)
  {
    if (std::get_if<Liquidation>(&e) != nullptr)
    {
      selfLiquidated = true;
    }
  }
  CHECK(!selfLiquidated);
  CHECK(eng.positionQty(1) == qty(5).raw());  // still open: not the engine's call

  // Seam 3: the owner closes it, and the engine settles exactly as its own
  // sweep would have.
  ev.clear();
  ForceClosePosition fc{};
  fc.accountId = 1;
  fc.symbol = SYM;
  eng.submit(InboundCommand{fc}, 4);
  CHECK(eng.positionQty(1) == 0);
  bool liquidated = false;
  for (auto& e : ev)
  {
    if (std::get_if<Liquidation>(&e) != nullptr)
    {
      liquidated = true;
    }
  }
  CHECK(liquidated);

  // Money is still conserved across the externally driven close.
  const Amount total = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(total == quote(20000));
}

TEST(Perp, EngineSuite)
{
  test_open_and_close();
  test_insufficient_margin();
  test_funding();
  test_liquidation();
  test_bankruptcy();
  test_liquidation_frees_resting_collateral();
  test_funding_triggers_liquidation();
  test_reduce_only_stop_cannot_open();
  test_perp_modify_respects_position_cap();
  test_perp_risk_gate_consistent_across_paths();
  test_perp_modify_preserves_reduce_only();
  test_perp_unpriced_sell_margin_matches_buy();
  test_reduce_only_resting_cannot_flip_at_fill();
  test_auction_uncross_respects_reduce_only();
  test_position_cap_not_circumvented_by_several_orders();
  test_engine_adl();
  test_position_limit();
  test_ledgerless_perp_tracks_exposure();
  test_ledgerless_money_commands_answer();
  test_external_risk_owner_seams();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
