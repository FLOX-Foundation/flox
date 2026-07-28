/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/ledger.h"
#include "flox-venue/matching_engine.h"
#include "flox/book/matching_book.h"

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
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg(int64_t llWindow = 0)
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.lastLookWindowNs = llWindow;
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
  int trades() const
  {
    int n = 0;
    for (auto& e : ev)
    {
      if (std::get_if<Trade>(&e))
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
    for (auto& e : ev)
    {
      if (std::get_if<T>(&e))
      {
        ++n;
      }
    }
    return n;
  }
  const FillHeld* firstHeld() const
  {
    for (auto& e : ev)
    {
      if (auto* h = std::get_if<FillHeld>(&e))
      {
        return h;
      }
    }
    return nullptr;
  }
};

void test_last_look()
{
  std::printf("test_last_look\n");
  {  // accept
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(/*window*/ 1000), cap.sink());
    NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, 0);
    eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
    CHECK(cap.trades() == 0);  // held, not traded
    CHECK(cap.count<FillHeld>() == 1);
    const auto* h = cap.firstHeld();
    CHECK(h && h->qty == qty(3));
    eng.submit(InboundCommand{LastLookDecision{h->heldId, SYM, true, 1}}, 2);
    CHECK(cap.trades() == 1);  // accepted -> traded
  }
  {  // reject
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(1000), cap.sink());
    NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, 0);
    eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);
    const auto* h = cap.firstHeld();
    eng.submit(InboundCommand{LastLookDecision{h->heldId, SYM, false, 1}}, 2);
    CHECK(cap.trades() == 0);
    CHECK(cap.count<FillRejected>() == 1);
  }
  {  // timeout -> reject (acceptOnTimeout defaults false)
    Cap cap;
    MatchingEngine<MatchingBook> eng(cfg(1000), cap.sink());
    NewOrder mk = limit(1, Side::SELL, 100, 5, 1);
    mk.lastLook = true;
    eng.submit(InboundCommand{mk}, 0);
    eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);  // deadline 1001
    eng.submit(InboundCommand{CancelOrder{999, SYM, 1}}, 5000);     // past deadline -> expire
    CHECK(cap.trades() == 0);
    CHECK(cap.count<FillRejected>() == 1);
  }
}

// Last-look reject/timeout must return BOTH parties' held buying power to
// available -- otherwise it is stranded in `reserved` forever and the taker is
// never made whole for a fill that never happened.
void test_last_look_reject_releases_reservation()
{
  std::printf("test_last_look_reject_releases_reservation\n");
  constexpr AssetId BASE = 0, QUOTE = 1;
  constexpr uint64_t VENUE = 999;
  const Amount base3 = amountOf(qty(3));
  const Amount usd300 = amountOf(Volume::fromDouble(300));
  Ledger led;
  led.deposit(1, BASE, base3);    // maker: 3 BTC
  led.deposit(2, QUOTE, usd300);  // taker: 300 USD
  auto c = cfg(/*window*/ 1000);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  Cap cap;
  MatchingEngine<MatchingBook> eng(c, cap.sink());
  eng.setLedger(&led, VENUE);
  NewOrder mk = limit(1, Side::SELL, 100, 3, 1);
  mk.lastLook = true;
  eng.submit(InboundCommand{mk}, 0);                              // maker reserves 3 BTC
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);  // taker reserves 300 USD; held 3
  const auto* h = cap.firstHeld();
  CHECK(h != nullptr);
  eng.submit(InboundCommand{LastLookDecision{h->heldId, SYM, false, 1}}, 2);  // reject
  CHECK(cap.trades() == 0);
  // Both parties fully made whole: reservations returned, no fill happened.
  CHECK(led.available(1, BASE) == base3 && led.reserved(1, BASE) == 0);     // maker
  CHECK(led.available(2, QUOTE) == usd300 && led.reserved(2, QUOTE) == 0);  // taker
  // Conservation: nothing created or destroyed.
  CHECK(led.total(1, BASE) == base3);
  CHECK(led.total(2, QUOTE) == usd300);
}

void test_mmp()
{
  std::printf("test_mmp\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.setMmp(/*account*/ 1, /*qtyLimit*/ qty(10), /*windowNs*/ 1000);
  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 20, 1)}, 0);  // MM rests 20
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 11, 2)}, 1);   // fills 11 from acct 1 -> breach
  CHECK(cap.count<MmpTriggered>() == 1);
  CHECK(eng.book().empty());  // account 1's remaining 9 was pulled
}

void test_mass_cancel()
{
  std::printf("test_mass_cancel\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5, 1)}, 0);
  eng.submit(InboundCommand{limit(2, Side::SELL, 101, 3, 1)}, 1);
  eng.submit(InboundCommand{limit(3, Side::BUY, 99, 2, 1)}, 2);
  eng.submit(InboundCommand{MassCancel{1, SYM}}, 3);
  CHECK(cap.count<OrderCanceled>() == 3);
  CHECK(eng.book().empty());
}

void test_quote()
{
  std::printf("test_quote\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  eng.submit(InboundCommand{Quote{10, 11, SYM, px(99), qty(5), px(101), qty(5), 1}}, 0);
  CHECK(eng.book().bestBid() == px(99));
  CHECK(eng.book().bestAsk() == px(101));
  // replace: tighter quote
  eng.submit(InboundCommand{Quote{10, 11, SYM, px(99.5), qty(4), px(100.5), qty(4), 1}}, 1);
  CHECK(eng.book().bestBid() == px(99.5));
  CHECK(eng.book().bestAsk() == px(100.5));
}

void test_fees()
{
  std::printf("test_fees\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  flox::FeeSchedule fs;
  fs.addTier(0.0, /*makerBps*/ -1.0, /*takerBps*/ 5.0);  // maker rebate, taker fee
  eng.setFeeSchedule(fs);
  eng.submit(InboundCommand{limit(1, Side::SELL, 100, 5, 1)}, 0);
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 3, 2)}, 1);  // trade notional 300
  double makerFee = 1e18, takerFee = 1e18;
  for (auto& e : cap.ev)
  {
    if (auto* f = std::get_if<FeeCharged>(&e))
    {
      if (f->maker)
      {
        makerFee = f->fee.toDouble();
      }
      else
      {
        takerFee = f->fee.toDouble();
      }
    }
  }
  CHECK(makerFee < 0.0);  // rebate
  CHECK(takerFee > 0.0);  // fee
}

void test_credit()
{
  std::printf("test_credit\n");
  Cap cap;
  MatchingEngine<MatchingBook> eng(cfg(), cap.sink());
  // Reject any order whose notional exceeds 1000 (toy buying-power gate).
  eng.setCreditCheck([](uint64_t, Side, Price p, Quantity q)
                     { return (p * q).toDouble() <= 1000.0; });
  eng.submit(InboundCommand{limit(1, Side::BUY, 100, 20, 1)}, 0);  // notional 2000 -> reject
  CHECK(cap.count<OrderRejected>() == 1);
  cap.ev.clear();
  eng.submit(InboundCommand{limit(2, Side::BUY, 100, 5, 1)}, 1);  // notional 500 -> ok
  CHECK(cap.count<OrderAccepted>() == 1);
}

}  // namespace

TEST(Mm, EngineSuite)
{
  test_last_look();
  test_last_look_reject_releases_reservation();
  test_mmp();
  test_mass_cancel();
  test_quote();
  test_fees();
  test_credit();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
