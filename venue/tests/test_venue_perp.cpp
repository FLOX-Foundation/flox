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
  test_perp_modify_preserves_reduce_only();
  test_engine_adl();
  test_position_limit();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
