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

#include "flox/backtest/fee_schedule.h"

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
constexpr AssetId BASE = 0;   // BTC
constexpr AssetId QUOTE = 1;  // USD
constexpr uint64_t VENUE = 999;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount base(double v) { return amountOf(qty(v)); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }

SymbolConfig cfg()
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

void test_settlement()
{
  std::printf("test_settlement\n");
  Ledger led;
  led.deposit(1, BASE, base(10));       // seller holds 10 base
  led.deposit(2, QUOTE, quote(10000));  // buyer holds 10000 quote

  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5, 1)}, 0);  // reserve 5 base
  CHECK(led.available(1, BASE) == base(5));
  CHECK(led.reserved(1, BASE) == base(5));

  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);  // trade 3 @100 = 300 quote

  // Buyer: paid 300 quote, received 3 base.
  CHECK(led.available(2, BASE) == base(3));
  CHECK(led.available(2, QUOTE) == quote(9700));
  CHECK(led.reserved(2, QUOTE) == 0);
  // Seller: delivered 3 base, received 300 quote; 2 base still reserved on the rest.
  CHECK(led.available(1, QUOTE) == quote(300));
  CHECK(led.reserved(1, BASE) == base(2));
  CHECK(led.total(1, BASE) == base(7));  // sold 3 of 10

  // Conservation: base and quote totals unchanged.
  CHECK(led.total(1, BASE) + led.total(2, BASE) == base(10));
  CHECK(led.total(1, QUOTE) + led.total(2, QUOTE) == quote(10000));

  // Cancel the seller remainder -> 2 base released.
  eng.submit(InboundCommand{CancelOrder{1, SYM, 1}}, 2);
  CHECK(led.available(1, BASE) == base(7));
  CHECK(led.reserved(1, BASE) == 0);
}

void test_insufficient()
{
  std::printf("test_insufficient\n");
  Ledger led;
  led.deposit(2, QUOTE, quote(100));  // only 100 quote
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 0);  // needs 300 quote
  bool rejected = false;
  for (auto& e : ev)
  {
    if (auto* r = std::get_if<OrderRejected>(&e); r && r->reason == RejectReason::InsufficientFunds)
    {
      rejected = true;
    }
  }
  CHECK(rejected);
  CHECK(led.available(2, QUOTE) == quote(100));  // nothing reserved
}

// A modify must re-check buying power: repricing up beyond funds is rejected,
// and shrinking frees the released reservation back to available.
void test_modify_reservation()
{
  std::printf("test_modify_reservation\n");
  Ledger led;
  led.deposit(2, QUOTE, quote(1000));  // funds a 10-lot bid at 100 (=1000)
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{limit(1, Side::BUY, 100, 10, 2)}, 0);  // reserves 1000
  CHECK(led.available(2, QUOTE) == 0 && led.reserved(2, QUOTE) == quote(1000));

  // Shrink to 4 lots: reservation drops to 400, 600 returns to available.
  eng.submit(InboundCommand{ModifyOrder{1, SYM, px(100), qty(4), 2}}, 1);
  CHECK(led.reserved(2, QUOTE) == quote(400));
  CHECK(led.available(2, QUOTE) == quote(600));

  // Reprice up to 100 @ 200 = 20000 needed, but only 1000 total -> rejected.
  ev.clear();
  eng.submit(InboundCommand{ModifyOrder{1, SYM, px(200), qty(100), 2}}, 2);
  bool rejected = false;
  for (auto& e : ev)
  {
    if (auto* r = std::get_if<OrderRejected>(&e); r && r->reason == RejectReason::InsufficientFunds)
    {
      rejected = true;
    }
  }
  CHECK(rejected);
  // Value conserved throughout (avail + reserved unchanged total).
  CHECK(led.available(2, QUOTE) + led.reserved(2, QUOTE) == quote(1000));
}

// Iceberg + modify-shrink: an iceberg's `leaves` is only the displayed peak,
// but reserveFunds reserves the FULL quantity. The same-price in-place shrink
// path must NOT proportionally release against the peak (it would over-release
// buying power) nor leave the hidden reserve resting. Icebergs re-enter, so a
// shrink to N leaves exactly N resting and reserves exactly N * price.
void test_iceberg_modify_shrink()
{
  std::printf("test_iceberg_modify_shrink\n");
  Ledger led;
  led.deposit(2, QUOTE, quote(1000));  // funds a 10-lot bid at 100
  led.deposit(1, BASE, base(10));      // a seller to fill against later
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);

  NewOrder ice = limit(1, Side::BUY, 100, 10, 2);
  ice.visibleQuantity = qty(2);  // show 2, hide 8; reserves the full 10 * 100
  eng.submit(InboundCommand{ice}, 0);
  CHECK(led.available(2, QUOTE) == 0 && led.reserved(2, QUOTE) == quote(1000));

  // Shrink to 1 lot. Buggy path freed 1000*(2-1)/2 = 500 (peak-relative) and left
  // 9 lots resting. Correct: full release + re-reserve 1 * 100 = 100.
  eng.submit(InboundCommand{ModifyOrder{1, SYM, px(100), qty(1), 2}}, 1);
  CHECK(led.reserved(2, QUOTE) == quote(100));
  CHECK(led.available(2, QUOTE) == quote(900));
  CHECK(led.available(2, QUOTE) + led.reserved(2, QUOTE) == quote(1000));

  // True remaining is 1, not 9: a seller dumping 10 @ 100 fills exactly 1.
  eng.submit(InboundCommand{limit(2, Side::SELL, 100, 10, 1)}, 2);
  CHECK(led.available(2, BASE) == base(1));      // bought exactly 1
  CHECK(led.reserved(2, QUOTE) == 0);            // reservation fully consumed/released
  CHECK(led.available(2, QUOTE) == quote(900));  // paid exactly 100
  // Conservation: no quote or base created.
  CHECK(led.total(1, QUOTE) + led.total(2, QUOTE) == quote(1000));
  CHECK(led.total(1, BASE) + led.total(2, BASE) == base(10));
}

// An IOC buy that partially fills must release the reservation for its UNFILLED
// residual -- otherwise buying power leaks (stuck in `reserved` forever).
void test_ioc_residual_release()
{
  std::printf("test_ioc_residual_release\n");
  Ledger led;
  led.deposit(1, BASE, base(3));       // seller: 3 base
  led.deposit(2, QUOTE, quote(1000));  // buyer: 1000 quote
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 3, 1)}, 0);  // rests 3 @ 100
  NewOrder b = limit(2, Side::BUY, 100, 10, 2);
  b.tif = TimeInForce::IOC;
  eng.submit(InboundCommand{b}, 1);  // reserves 1000, fills 3 (=300), residual 7 canceled

  // 300 spent on the fill; the 700 residual reservation must be back in available.
  CHECK(led.reserved(2, QUOTE) == 0);            // no leak
  CHECK(led.available(2, QUOTE) == quote(700));  // 1000 - 300 settled
}

// A triggered stop settles via the unreserved-debit path. If the taker can't
// afford the fill, value must NOT be created (buyer must not receive base it
// didn't pay for). Conservation must hold even for underfunded triggered stops.
void test_stop_underfunded()
{
  std::printf("test_stop_underfunded\n");
  Ledger led;
  led.deposit(1, BASE, base(100));       // seller: plenty of base
  led.deposit(3, QUOTE, quote(50));      // stop buyer: only 50 quote (can't afford 10@100=1000)
  led.deposit(9, QUOTE, quote(100000));  // a funded taker to set the last price
  led.deposit(9, BASE, base(100));
  const Amount initBase = led.total(1, BASE) + led.total(3, BASE) + led.total(9, BASE) + led.total(VENUE, BASE);
  const Amount initQuote =
      led.total(1, QUOTE) + led.total(3, QUOTE) + led.total(9, QUOTE) + led.total(VENUE, QUOTE);

  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 100, 1)}, 0);  // resting sell 100 @ 100
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 1, 9)}, 1);     // funded buy 1 -> last = 100

  // Underfunded stop-market buy, trigger 99 (last=100 >= 99 -> fires immediately).
  NewOrder st;
  st.id = 3;
  st.symbol = SYM;
  st.side = Side::BUY;
  st.type = OrderType::STOP_MARKET;
  st.triggerPrice = px(99);
  st.quantity = qty(10);
  st.accountId = 3;
  eng.submit(InboundCommand{st}, 2);

  // Whatever filled, value is conserved: the buyer never receives base it didn't
  // pay for (no money created), across all accounts + venue.
  const Amount endBase = led.total(1, BASE) + led.total(3, BASE) + led.total(9, BASE) + led.total(VENUE, BASE);
  const Amount endQuote =
      led.total(1, QUOTE) + led.total(3, QUOTE) + led.total(9, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(endBase == initBase);
  CHECK(endQuote == initQuote);
  CHECK(led.available(3, QUOTE) >= 0);  // buyer never goes negative
}

void test_fees_to_venue()
{
  std::printf("test_fees_to_venue\n");
  Ledger led;
  led.deposit(1, BASE, base(10));
  led.deposit(2, QUOTE, quote(10000));
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);
  flox::FeeSchedule fs;
  fs.addTier(0.0, /*makerBps*/ 1.0, /*takerBps*/ 2.0);  // both pay
  eng.setFeeSchedule(fs);

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 3, 1)}, 0);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);  // notional 300

  // maker fee 300*1bp=0.03, taker fee 300*2bp=0.06 -> venue gets 0.09 quote.
  CHECK(led.available(VENUE, QUOTE) == quote(0.09));
  // Value conserved across participants + venue.
  const Amount totalQuote =
      led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(totalQuote == quote(10000));
}

// Fee policy: the buying-power gate reserves the NOTIONAL; the taker fee is
// charged post-settlement. A taker funded to exactly the notional therefore
// ends in a fee-sized deficit (which blocks further trading until topped up),
// rather than being pre-rejected -- a standard post-fill-fee model. The hard
// invariants that MUST hold: value is conserved, the venue receives the fee,
// and any deficit is bounded by the fee (never an unbounded overdraw).
void test_taker_fee_policy()
{
  std::printf("test_taker_fee_policy\n");
  Ledger led;
  led.deposit(1, BASE, base(3));
  led.deposit(2, QUOTE, quote(300));  // buyer: EXACTLY the 3@100 notional, nothing spare
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);
  flox::FeeSchedule fs;
  fs.addTier(0.0, /*maker*/ 0.0, /*taker*/ 2.0);  // 2bp taker fee = 0.06 on 300
  eng.setFeeSchedule(fs);

  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 3, 1)}, 0);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);

  const Amount fee = quote(0.06);
  CHECK(led.total(VENUE, QUOTE) == fee);   // venue captured the taker fee
  CHECK(led.available(2, QUOTE) == -fee);  // deficit bounded by the fee, not unbounded
  // Conservation across buyer + seller + venue (base and quote).
  CHECK(led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE) == quote(300));
  CHECK(led.total(1, BASE) + led.total(2, BASE) == base(3));
}

// Opening-auction settlement with a ledger: orders reserve during pre-open,
// then the uncross settles through the same ledger. Value must be conserved and
// no reservation may leak after the book is drained.
void test_auction_settlement()
{
  std::printf("test_auction_settlement\n");
  Ledger led;
  led.deposit(1, BASE, base(100));
  led.deposit(2, QUOTE, quote(100000));
  led.deposit(3, QUOTE, quote(100000));
  const Amount initBase = led.total(1, BASE) + led.total(2, BASE) + led.total(3, BASE) + led.total(VENUE, BASE);
  const Amount initQuote =
      led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(3, QUOTE) + led.total(VENUE, QUOTE);

  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);
  eng.beginPreOpen();
  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 10, 1)}, 0);  // accumulate (reserve base)
  eng.submit(InboundCommand{limit(2, Side::BUY, 101, 6, 2)}, 1);    // crosses, but only accumulates
  eng.submit(InboundCommand{limit(3, Side::BUY, 100, 8, 3)}, 2);
  eng.openContinuous();  // uncross at the single price -> settles through the ledger

  // Value conserved across all participants + venue after the uncross.
  const Amount endBase = led.total(1, BASE) + led.total(2, BASE) + led.total(3, BASE) + led.total(VENUE, BASE);
  const Amount endQuote =
      led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(3, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(endBase == initBase);
  CHECK(endQuote == initQuote);

  // Drain the book; no reservation may leak.
  for (OrderId id = 1; id <= 3; ++id)
  {
    eng.submit(InboundCommand{CancelOrder{id, SYM, 0}}, 3);
  }
  int leaks = 0;
  for (uint64_t a = 1; a <= 3; ++a)
  {
    if (led.reserved(a, BASE) != 0 || led.reserved(a, QUOTE) != 0)
    {
      ++leaks;
    }
  }
  CHECK(leaks == 0);
}

// A leg with no reservation settles straight out of `available`, and that debit
// can refuse. Crediting the counterparty anyway mints exactly the credited
// amount -- the failure mode behind T028. This is the floor under EVERY such
// path, not just the one that was found: an order that reached the book before
// a ledger was bound has no reservation, so its fill takes the unreserved
// branch with an empty account behind it. Nothing may move, and the venue must
// count the trade as unsettled rather than pay for it.
void test_unreserved_leg_cannot_mint()
{
  std::printf("test_unreserved_leg_cannot_mint\n");
  Ledger led;
  led.deposit(2, QUOTE, quote(1000));  // buyer funded; seller owns no base at all
  const Amount initBase = led.total(1, BASE) + led.total(2, BASE) + led.total(VENUE, BASE);
  const Amount initQuote = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);

  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5, 1)}, 0);  // no ledger yet: no reservation
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 5, 2)}, 1);  // reserved buyer takes it

  CHECK(eng.unsettledTrades() == 1);  // the print could not be settled honestly
  // Nothing moved: no base conjured for the buyer, no quote paid to the seller.
  CHECK(led.total(1, BASE) == 0 && led.total(2, BASE) == 0);
  CHECK(led.total(1, QUOTE) == 0);
  const Amount endBase = led.total(1, BASE) + led.total(2, BASE) + led.total(VENUE, BASE);
  const Amount endQuote = led.total(1, QUOTE) + led.total(2, QUOTE) + led.total(VENUE, QUOTE);
  CHECK(endBase == initBase);
  CHECK(endQuote == initQuote);
  // The buyer's own money is intact and free again (its order left the book).
  CHECK(led.available(2, QUOTE) == quote(1000) && led.reserved(2, QUOTE) == 0);
}

}  // namespace

// ---- three defects that used to be "accepted" ------------------------------

// STP Decrement trims a resting order in place. The reservation covering the
// trimmed quantity used to stay locked until the order was cancelled: the
// account's own money, frozen against size that no longer rests.
void test_stp_decrement_frees_trimmed_reservation()
{
  std::printf("test_stp_decrement_frees_trimmed_reservation\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(10000));
  led.deposit(1, BASE, base(10));
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setLedger(&led, VENUE);
  eng.setStpGroup(1, 1);  // same firm on both sides

  NewOrder resting = limit(1, Side::SELL, 100, 5, 1);
  resting.stp = STPMode::Decrement;
  eng.submit(InboundCommand{resting}, 1);
  const Amount reservedAfterRest = led.reserved(1, BASE);
  CHECK(reservedAfterRest == base(5));

  // Same firm crosses with 2: STP decrements the resting side to 3.
  NewOrder crossing = limit(2, Side::BUY, 100, 2, 1);
  crossing.stp = STPMode::Decrement;
  eng.submit(InboundCommand{crossing}, 2);
  CHECK(led.reserved(1, BASE) == base(3));  // was still 5 -- 2 units frozen for nothing
  CHECK(led.available(1, BASE) + led.reserved(1, BASE) == base(10));
}

// A GTD conditional order used to live forever: expiry was registered only for
// orders resting on the book, so a stop that never triggered never expired.
void test_gtd_binds_conditional_orders()
{
  std::printf("test_gtd_binds_conditional_orders\n");
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });

  NewOrder stop = limit(1, Side::SELL, 0, 1, 1);
  stop.type = OrderType::STOP_MARKET;
  stop.triggerPrice = px(50);  // far from any print: never triggers
  stop.tif = TimeInForce::GTD;
  stop.expiryNs = 1000;
  eng.submit(InboundCommand{stop}, 10);

  // Any later command drives the deterministic sweep past the deadline.
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 1, 2)}, 2000);
  bool expired = false;
  for (auto& e : ev)
  {
    if (auto* c = std::get_if<OrderCanceled>(&e);
        c != nullptr && c->id == 1 && c->reason == CancelReason::Expired)
    {
      expired = true;
    }
  }
  CHECK(expired);
}

TEST(VenueLedger, EngineSuite)
{
  test_unreserved_leg_cannot_mint();
  test_settlement();
  test_taker_fee_policy();
  test_stp_decrement_frees_trimmed_reservation();
  test_gtd_binds_conditional_orders();
  test_auction_settlement();
  test_insufficient();
  test_modify_reservation();
  test_iceberg_modify_shrink();
  test_ioc_residual_release();
  test_stop_underfunded();
  test_fees_to_venue();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
