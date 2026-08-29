/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Money-conservation fuzz over the venue core. Value must never be created or
 * destroyed across a random command stream (spot, perp, perp+ADL, cross-margin,
 * multi-asset collateral), and after the book is drained every account's
 * `reserved` must return to zero -- a stuck reservation is invisible to
 * conservation-of-total but shows up in that second check.
 */
#include "flox-venue/collateral.h"
#include "flox-venue/cross_margin.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include "flox/backtest/fee_schedule.h"

#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cstdio>
#include <cstdlib>

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
constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE = 9;
constexpr int NACCT = 8;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount base(double v) { return amountOf(qty(v)); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }

struct Rng
{
  uint64_t s;
  uint64_t next()
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

void test_spot_conservation()
{
  std::printf("test_spot_conservation\n");
  Ledger led;
  for (int a = 1; a <= NACCT; ++a)
  {
    led.deposit(a, BASE, base(1000));
    led.deposit(a, QUOTE, quote(100000));
  }
  const Amount initBase = base(1000) * NACCT;
  const Amount initQuote = quote(100000) * NACCT;

  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50);
  c.maxPrice = px(150);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  MatchingEngine<MatchingBook> eng(c, [](const OutboundEvent&) {});
  flox::FeeSchedule fs;
  fs.addTier(0.0, -1.0, 2.0);  // maker rebate, taker fee -> venue nets fees
  eng.setFeeSchedule(fs);
  eng.setLedger(&led, VENUE);

  auto sumAsset = [&](AssetId asset)
  {
    Amount t = 0;
    for (int a = 1; a <= NACCT; ++a)
    {
      t += led.total(a, asset);
    }
    t += led.total(VENUE, asset);
    return t;
  };

  Rng rng{0xDEADBEEF12345ULL};
  const int64_t midRaw = px(100).raw();
  const int64_t tickRaw = px(0.01).raw();
  int breaches = 0;
  OrderId nextId = 1;
  for (int i = 0; i < 200000; ++i)
  {
    const uint64_t r = rng.next();
    const uint32_t kind = r % 100;
    if (kind < 20 && nextId > 1)
    {
      eng.submit(InboundCommand{CancelOrder{1 + (rng.next() % (nextId - 1)), SYM, 0}}, i);
    }
    else if (kind < 30 && nextId > 1)
    {
      // Modify a random existing order (reprice + resize) -- must adjust the
      // ledger reservation, else buying power leaks/double-counts.
      const OrderId vid = 1 + (rng.next() % (nextId - 1));
      const int ticks = static_cast<int>((r >> 1) % 101) - 50;
      const Price np = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
      const Quantity nq = qty(1.0 + static_cast<double>((r >> 24) % 5));
      eng.submit(InboundCommand{ModifyOrder{vid, SYM, np, nq, 0}}, i);
    }
    else
    {
      NewOrder o;
      o.id = nextId++;
      o.symbol = SYM;
      o.side = (r & 1) ? Side::BUY : Side::SELL;
      o.accountId = 1 + (r >> 8) % NACCT;
      const int ticks = static_cast<int>((r >> 1) % 101) - 50;
      o.price = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
      o.quantity = qty(1.0 + static_cast<double>((r >> 20) % 5));
      o.type = (kind < 25) ? OrderType::MARKET : OrderType::LIMIT;
      if (kind >= 25 && kind < 32)  // stop-market: reserve-at-trigger + settle path
      {
        o.type = OrderType::STOP_MARKET;
        const int tt = static_cast<int>((r >> 33) % 61) - 30;
        o.triggerPrice = Price::fromRaw(midRaw + static_cast<int64_t>(tt) * tickRaw);
      }
      // Exercise GTD expiry, OCO cancellation, and peg reprice under the ledger
      // conservation invariant (each must release/re-reserve buying power).
      if (o.type == OrderType::LIMIT)
      {
        const uint32_t t = static_cast<uint32_t>((r >> 32) % 100);
        if (t < 15)
        {
          o.tif = TimeInForce::GTD;
          o.expiryNs = SeqNanos::fromRaw(i + 1 + static_cast<int64_t>((r >> 40) % 50));  // expires soon
        }
        else if (t < 25)
        {
          o.ocoGroup = 1 + ((r >> 44) % 5);
        }
        else if (t < 35)
        {
          const uint32_t pr = static_cast<uint32_t>((r >> 48) % 3);
          o.peg = pr == 0 ? PegRef::Bid : (pr == 1 ? PegRef::Ask : PegRef::Mid);
        }
        else if (t < 45)
        {
          o.tif = TimeInForce::IOC;  // partial fill + residual cancel must release reserve
        }
        else if (t < 52)
        {
          o.tif = TimeInForce::FOK;  // all-or-nothing: full reserve released on kill
        }
        else if (t < 58)
        {
          o.postOnly = true;  // maker-only: rejected-on-cross must release reserve
        }
        else if (t < 66)
        {
          // Iceberg: reserves the FULL qty but shows only a peak. Adds peak
          // refill + hidden-reserve matching to the fuzz. NOTE: a per-order
          // reservation bug (over-release against the peak on modify) is NOT
          // visible here -- the ledger aggregates `reserved` per account, so one
          // order's over-release is masked by other orders. The precise guard is
          // the single-order test_iceberg_modify_shrink in test_ledger.
          o.visibleQuantity = qty(1.0 + static_cast<double>((r >> 52) % 2));
        }
      }
      eng.submit(InboundCommand{o}, i);
    }
    if (sumAsset(BASE) != initBase || sumAsset(QUOTE) != initQuote)
    {
      ++breaches;
      if (breaches == 1)
      {
        std::printf("  first breach at op %d\n", i);
      }
    }
  }
  CHECK(breaches == 0);

  // Reserved-invariant: drain the whole book, then EVERY account's reserved must
  // return to zero. A stuck reservation (leak) is invisible to conservation-of-
  // total but shows up here as residual `reserved` after all orders are gone.
  for (OrderId id = 1; id < nextId; ++id)
  {
    eng.submit(InboundCommand{CancelOrder{id, SYM, 0}}, 999999);
  }
  int reservedLeaks = 0;
  for (int a = 1; a <= NACCT; ++a)
  {
    if (led.reserved(a, BASE) != 0 || led.reserved(a, QUOTE) != 0)
    {
      ++reservedLeaks;
    }
  }
  CHECK(reservedLeaks == 0);
  std::printf("  200000 ops, base/quote conserved (%d breaches); reserved drained clean (%d leaks)\n",
              breaches, reservedLeaks);
}

// Last-look lifecycle under the conservation invariant: a slice of the flow is
// lastLook makers, and holds resolve through every path -- explicit accepts,
// explicit rejects, wrong-owner decision attempts, timeout (acceptOnTimeout
// exercises the timeout-accept settle), cancel-while-held during the drain, and
// SELF-TRADE PREVENTION removing a maker that has a hold open (T028: the one
// removal path that lives inside the matcher, where a cancel that ignored the
// hold used to free the collateral the later accept had to settle from). Money
// must never be created or destroyed, and after the drain every reservation
// (including restored held slices) must return to zero.
void test_spot_conservation_lastlook()
{
  std::printf("test_spot_conservation_lastlook\n");
  Ledger led;
  for (int a = 1; a <= NACCT; ++a)
  {
    led.deposit(a, BASE, base(1000));
    led.deposit(a, QUOTE, quote(100000));
  }
  const Amount initBase = base(1000) * NACCT;
  const Amount initQuote = quote(100000) * NACCT;

  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50);
  c.maxPrice = px(150);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  c.lastLookWindowNs = DurationNs{40};  // fuzz time = op index, so holds expire quickly
  c.lastLookAcceptOnTimeout = true;     // timeout-accept settles like a real fill
  std::unordered_map<OrderId, uint64_t> acctOf;
  std::vector<std::pair<uint64_t, uint64_t>> pendingHolds;  // (heldId, makerAccount)
  uint64_t totalHolds = 0;
  uint64_t stpCancels = 0;
  uint64_t stpCancelsOnHeldMaker = 0;  // T028 coverage: STP pulled a maker mid-hold
  OrderId lastRejectedMaker = 0;
  MatchingEngine<MatchingBook> eng(
      c,
      [&](const OutboundEvent& e)
      {
        if (const auto* h = std::get_if<FillHeld>(&e))
        {
          ++totalHolds;
          pendingHolds.emplace_back(h->heldId, acctOf[h->makerId]);
        }
        else if (const auto* fr = std::get_if<FillRejected>(&e))
        {
          lastRejectedMaker = fr->makerId;  // a hold on this maker just resolved
        }
        else if (const auto* oc = std::get_if<OrderCanceled>(&e);
                 oc != nullptr && oc->reason == CancelReason::SelfTradePrevention)
        {
          ++stpCancels;
          // The STP path resolves the maker's holds immediately before pulling
          // it, so this pairing is exactly the interaction under test.
          if (oc->id == lastRejectedMaker)
          {
            ++stpCancelsOnHeldMaker;
          }
        }
      });
  flox::FeeSchedule fs;
  fs.addTier(0.0, -1.0, 2.0);
  eng.setFeeSchedule(fs);
  eng.setLedger(&led, VENUE);

  auto sumAsset = [&](AssetId asset)
  {
    Amount t = 0;
    for (int a = 1; a <= NACCT; ++a)
    {
      t += led.total(a, asset);
    }
    t += led.total(VENUE, asset);
    return t;
  };

  Rng rng{0xFEEDFACE77ULL};
  const int64_t midRaw = px(100).raw();
  const int64_t tickRaw = px(0.01).raw();
  int breaches = 0;
  OrderId nextId = 1;
  for (int i = 0; i < 100000; ++i)
  {
    const uint64_t r = rng.next();
    const uint32_t kind = r % 100;
    if (kind < 12 && !pendingHolds.empty())
    {
      // Resolve a random pending hold: correct owner half the time (accept or
      // reject), a random -- usually wrong -- account otherwise (NotOrderOwner
      // reject path, the hold stays pending until timeout).
      const size_t pick = (r >> 16) % pendingHolds.size();
      const auto [hid, makerAcct] = pendingHolds[pick];
      pendingHolds.erase(pendingHolds.begin() + static_cast<ptrdiff_t>(pick));
      const uint64_t acct = (r & 4) ? makerAcct : 1 + ((r >> 24) % NACCT);
      eng.submit(InboundCommand{LastLookDecision{hid, SYM, (r & 8) != 0, acct}}, i);
    }
    else if (kind < 27 && nextId > 1)
    {
      eng.submit(InboundCommand{CancelOrder{1 + (rng.next() % (nextId - 1)), SYM, 0}}, i);
    }
    else
    {
      NewOrder o;
      o.id = nextId++;
      o.symbol = SYM;
      o.side = (r & 1) ? Side::BUY : Side::SELL;
      o.accountId = 1 + (r >> 8) % NACCT;
      const int ticks = static_cast<int>((r >> 1) % 101) - 50;
      o.price = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
      o.quantity = qty(1.0 + static_cast<double>((r >> 20) % 5));
      o.type = (kind < 32) ? OrderType::MARKET : OrderType::LIMIT;
      if (o.type == OrderType::LIMIT)
      {
        const uint32_t t = static_cast<uint32_t>((r >> 32) % 100);
        if (t < 35)
        {
          o.lastLook = true;  // a third of the resting flow quotes with last look
        }
        else if (t < 45)
        {
          o.tif = TimeInForce::IOC;  // held IOC residual must cancel + release
        }
        else if (t < 52)
        {
          o.tif = TimeInForce::FOK;  // FOK never executes into last-look range
        }
      }
      // Self-trade prevention on a slice of the aggressive flow, ON TOP of the
      // last-look makers above: with 8 accounts an aggressor regularly crosses
      // its own quote, so the matcher's own cancel path meets held makers. All
      // four modes, since each removes or reshapes the maker differently.
      if (!o.lastLook)
      {
        switch (static_cast<uint32_t>((r >> 56) % 8))
        {
          case 0:
            o.stp = STPMode::CancelOldest;
            break;
          case 1:
            o.stp = STPMode::CancelNewest;
            break;
          case 2:
            o.stp = STPMode::CancelBoth;
            break;
          case 3:
            o.stp = STPMode::Decrement;
            break;
          default:
            break;  // the rest trade through their own quotes as before
        }
      }
      acctOf[o.id] = o.accountId;
      eng.submit(InboundCommand{o}, i);
    }
    if (sumAsset(BASE) != initBase || sumAsset(QUOTE) != initQuote)
    {
      ++breaches;
      if (breaches == 1)
      {
        std::printf("  first breach at op %d\n", i);
      }
    }
  }
  CHECK(breaches == 0);

  // Drain: cancel everything (cancel-while-held resolves any hold left), then
  // every account's reserved must be zero -- a held slice whose reservation
  // was stripped or stranded shows up here.
  for (OrderId id = 1; id < nextId; ++id)
  {
    eng.submit(InboundCommand{CancelOrder{id, SYM, 0}}, 999999);
  }
  CHECK(sumAsset(BASE) == initBase && sumAsset(QUOTE) == initQuote);
  int reservedLeaks = 0;
  for (int a = 1; a <= NACCT; ++a)
  {
    if (led.reserved(a, BASE) != 0 || led.reserved(a, QUOTE) != 0)
    {
      ++reservedLeaks;
    }
  }
  CHECK(reservedLeaks == 0);
  CHECK(eng.unsettledTrades() == 0);  // no fill ever reached clearing unbacked
  CHECK(totalHolds > 1000);           // coverage guard: the scenario really exercised holds
  CHECK(stpCancels > 100);            // ... and that STP really fired alongside them
  CHECK(stpCancelsOnHeldMaker > 0);   // ... including on makers with a live hold (T028)
  std::printf(
      "  100000 ops, %llu holds (accept/reject/wrong-owner/timeout-accept/cancel-while-held), "
      "%llu STP cancels (%llu on a held maker), base/quote conserved (%d breaches); reserved "
      "drained clean (%d leaks)\n",
      static_cast<unsigned long long>(totalHolds), static_cast<unsigned long long>(stpCancels),
      static_cast<unsigned long long>(stpCancelsOnHeldMaker), breaches, reservedLeaks);
}

void test_perp_conservation(bool adl)
{
  std::printf("test_perp_conservation%s\n", adl ? " (ADL)" : "");
  Ledger led;
  for (int a = 1; a <= NACCT; ++a)
  {
    led.deposit(a, QUOTE, quote(1000000));
  }
  if (adl)
  {
    led.deposit(VENUE, QUOTE, quote(1000000));  // seed insurance fund
  }
  Amount initQuote = quote(1000000) * NACCT;
  if (adl)
  {
    initQuote += quote(1000000);
  }

  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50);
  c.maxPrice = px(150);
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;
  c.maintenanceMarginBps = 500;
  c.autoDeleverage = adl;
  c.maxPositionQty = qty(20);  // the cap must bind the RESULTING position (T030)
  MatchingEngine<MatchingBook> eng(c, [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);

  auto sumQuote = [&]()
  {
    Amount t = 0;
    for (int a = 1; a <= NACCT; ++a)
    {
      t += led.total(a, QUOTE);
    }
    t += led.total(VENUE, QUOTE);
    return t;
  };

  Rng rng{0x1234ABCDULL};
  const int64_t midRaw = px(100).raw();
  const int64_t tickRaw = px(0.01).raw();
  int breaches = 0;
  int capBreaches = 0;
  OrderId nextId = 1;
  for (int i = 0; i < 100000; ++i)
  {
    const uint64_t r = rng.next();
    const uint32_t kind = r % 100;
    if (kind < 15 && nextId > 1)
    {
      eng.submit(InboundCommand{CancelOrder{1 + (rng.next() % (nextId - 1)), SYM, 0}}, i);
    }
    else if (kind < 25)
    {
      // periodic mark move -> may trigger liquidations
      const int ticks = static_cast<int>((r >> 1) % 61) - 30;
      eng.setMarkPrice(Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw));
    }
    else
    {
      NewOrder o;
      o.id = nextId++;
      o.symbol = SYM;
      o.side = (r & 1) ? Side::BUY : Side::SELL;
      o.accountId = 1 + (r >> 8) % NACCT;
      const int ticks = static_cast<int>((r >> 1) % 61) - 30;
      o.price = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
      o.quantity = qty(1.0 + static_cast<double>((r >> 20) % 5));
      // Unpriced flow: a perp market/stop order is margined against the price
      // BAND, not its own price, and the band bound must be the same on both
      // sides (T029) -- a LIMIT-only fuzz never touches that arithmetic.
      const uint32_t t = static_cast<uint32_t>((r >> 32) % 100);
      if (t < 15)
      {
        o.type = OrderType::MARKET;
      }
      else if (t < 25)
      {
        o.type = OrderType::STOP_MARKET;
        const int tt = static_cast<int>((r >> 40) % 61) - 30;
        o.triggerPrice = Price::fromRaw(midRaw + static_cast<int64_t>(tt) * tickRaw);
      }
      else if (t < 32)
      {
        o.type = OrderType::STOP_LIMIT;
        const int tt = static_cast<int>((r >> 40) % 61) - 30;
        o.triggerPrice = Price::fromRaw(midRaw + static_cast<int64_t>(tt) * tickRaw);
      }
      // Reduce-only flow: capped at admission, re-measured at fill time. It
      // reserves no margin, so a slice of it that opened a position would open
      // one with none (T030).
      o.reduceOnly = ((r >> 48) % 100) < 20;
      eng.submit(InboundCommand{o}, i);
    }
    if (sumQuote() != initQuote)
    {
      ++breaches;
      if (breaches == 1)
      {
        std::printf("  first breach at op %d\n", i);
      }
    }
    // The position cap binds the RESULT, not just the incoming order: no
    // account may ever be carried past it by fills.
    for (int a = 1; a <= NACCT; ++a)
    {
      const int64_t q = eng.positionQty(a);
      if ((q < 0 ? -q : q) > qty(20).raw())
      {
        ++capBreaches;
        if (capBreaches == 1)
        {
          std::printf("  first position-cap breach at op %d (acct %d, qty %lld)\n", i, a,
                      static_cast<long long>(q));
        }
      }
    }
  }
  CHECK(breaches == 0);
  CHECK(capBreaches == 0);
  CHECK(eng.unsettledTrades() == 0);

  // Perp reserved-invariant: drain resting orders, then every reserved quote
  // unit must be backed by an open position's posted margin -- no IM leak.
  for (OrderId id = 1; id < nextId; ++id)
  {
    eng.submit(InboundCommand{CancelOrder{id, SYM, 0}}, 999999);
  }
  Amount reservedSum = 0;
  for (int a = 1; a <= NACCT; ++a)
  {
    reservedSum += led.reserved(a, QUOTE);
  }
  CHECK(reservedSum == eng.totalPositionMargin());
  std::printf(
      "  100000 ops (limit+market+stop+reduce-only orders, marks, liquidations), collateral "
      "conserved (%d breaches); position cap held (%d breaches); reserved==position-margin (%s)\n",
      breaches, capBreaches, reservedSum == eng.totalPositionMargin() ? "ok" : "LEAK");
}

void test_cross_margin_conservation()
{
  std::printf("test_cross_margin_conservation\n");
  Ledger led;
  for (int a = 1; a <= NACCT; ++a)
  {
    led.deposit(a, QUOTE, quote(1000000));
  }
  const Amount initQuote = quote(1000000) * NACCT;

  CrossMarginManager m(led, QUOTE, VENUE);
  constexpr SymbolId S1 = 1;
  constexpr SymbolId S2 = 2;
  m.configureSymbol(S1, /*im*/ 1000, /*mm*/ 500);
  m.configureSymbol(S2, /*im*/ 2000, /*mm*/ 1000);
  m.setMark(S1, px(100));
  m.setMark(S2, px(100));

  auto sumQuote = [&]()
  {
    Amount t = 0;
    for (int a = 1; a <= NACCT; ++a)
    {
      t += led.total(a, QUOTE);
    }
    t += led.total(VENUE, QUOTE);
    return t;
  };

  Rng rng{0xC0FFEE99ULL};
  const int64_t midRaw = px(100).raw();
  const int64_t tickRaw = px(0.01).raw();
  int breaches = 0;
  for (int i = 0; i < 100000; ++i)
  {
    const uint64_t r = rng.next();
    const uint32_t kind = r % 100;
    const SymbolId sym = (r & 2) ? S2 : S1;
    if (kind < 20)
    {
      // mark move -> may liquidate underwater portfolios
      const int ticks = static_cast<int>((r >> 1) % 61) - 30;
      m.setMark(sym, Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw));
    }
    else if (kind < 25)
    {
      // periodic funding on the portfolio book -- must conserve
      const int ticks = static_cast<int>((r >> 1) % 61) - 30;
      m.applyFunding(sym, 0.0005, Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw));
    }
    else
    {
      uint64_t buyer = 1 + (r >> 8) % NACCT;
      uint64_t seller = 1 + (r >> 16) % NACCT;
      if (buyer == seller)
      {
        seller = 1 + (seller % NACCT);
      }
      const int ticks = static_cast<int>((r >> 1) % 61) - 30;
      const int64_t priceRaw = midRaw + static_cast<int64_t>(ticks) * tickRaw;
      const int64_t q = qty(1.0 + static_cast<double>((r >> 24) % 5)).raw();
      // Both sides settle through the same pool -> value conserved by construction.
      m.applyFill(buyer, sym, Side::BUY, q, priceRaw);
      m.applyFill(seller, sym, Side::SELL, q, priceRaw);
    }
    if (sumQuote() != initQuote)
    {
      ++breaches;
      if (breaches == 1)
      {
        std::printf("  first breach at op %d\n", i);
      }
    }
  }
  CHECK(breaches == 0);
  std::printf(
      "  100000 ops (2-symbol cross-margin fills+marks+funding+liquidations), collateral "
      "conserved (%d breaches)\n",
      breaches);
}

void test_multi_collateral_conservation()
{
  std::printf("test_multi_collateral_conservation\n");
  constexpr AssetId BTC_COL = 2;  // collateral asset (distinct from the perp symbol)
  Ledger led;
  for (int a = 1; a <= NACCT; ++a)
  {
    led.deposit(a, QUOTE, quote(500));                           // thin quote
    led.deposit(a, BTC_COL, amountOf(Quantity::fromDouble(2)));  // + coin collateral
  }
  led.deposit(VENUE, QUOTE, quote(100000000));  // deep insurance
  const Amount initQuote = quote(500) * NACCT + quote(100000000);
  const Amount initBtc = amountOf(Quantity::fromDouble(2)) * NACCT;

  CollateralSchedule sched;
  sched.configure(QUOTE, px(1.0).raw(), 0);
  sched.configure(BTC_COL, px(300).raw(), 1500);  // coin priced at 300, 15% haircut

  CrossMarginManager m(led, QUOTE, VENUE, [](const Liquidation&) {}, /*adl*/ false);
  m.setCollateralSchedule(&sched);
  m.configureSymbol(SYM, /*im*/ 1000, /*mm*/ 500);
  m.setMark(SYM, px(100));

  auto sumAsset = [&](AssetId asset)
  {
    Amount t = led.total(VENUE, asset);
    for (int a = 1; a <= NACCT; ++a)
    {
      t += led.total(a, asset);
    }
    return t;
  };

  Rng rng{0xC01A7E5ULL};
  const int64_t midRaw = px(100).raw();
  const int64_t tickRaw = px(0.01).raw();
  int breaches = 0;
  for (int i = 0; i < 100000; ++i)
  {
    const uint64_t r = rng.next();
    const uint32_t kind = r % 100;
    if (kind < 20)
    {
      const int ticks = static_cast<int>((r >> 1) % 61) - 30;
      m.setMark(SYM, Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw));
    }
    else if (kind < 25)
    {
      // re-price the coin collateral (revaluation shifts who is liquidatable)
      const int d = static_cast<int>((r >> 3) % 201) - 100;  // +/- 100
      sched.setPrice(BTC_COL, px(300 + d).raw());
    }
    else
    {
      uint64_t buyer = 1 + (r >> 8) % NACCT;
      uint64_t seller = 1 + (r >> 16) % NACCT;
      if (buyer == seller)
      {
        seller = 1 + (seller % NACCT);
      }
      const int ticks = static_cast<int>((r >> 1) % 61) - 30;
      const int64_t priceRaw = midRaw + static_cast<int64_t>(ticks) * tickRaw;
      const int64_t q = qty(1.0 + static_cast<double>((r >> 24) % 5)).raw();
      m.applyFill(buyer, SYM, Side::BUY, q, priceRaw);
      m.applyFill(seller, SYM, Side::SELL, q, priceRaw);
    }
    if (sumAsset(QUOTE) != initQuote || sumAsset(BTC_COL) != initBtc)
    {
      ++breaches;
      if (breaches == 1)
      {
        std::printf("  first breach at op %d\n", i);
      }
    }
  }
  CHECK(breaches == 0);
  std::printf(
      "  100000 ops (2-asset collateral: fills+marks+revaluation+collateral-liquidation), "
      "quote+coin conserved (%d breaches)\n",
      breaches);
}

}  // namespace

TEST(VenueConservationFuzz, SpotValueAndReservationsConserved)
{
  g_failures = 0;  // independent baseline: a prior scenario must not taint this one
  test_spot_conservation();
  EXPECT_EQ(g_failures, 0);
}

TEST(VenueConservationFuzz, SpotLastLookConserved)
{
  g_failures = 0;  // independent baseline: a prior scenario must not taint this one
  test_spot_conservation_lastlook();
  EXPECT_EQ(g_failures, 0);
}

TEST(VenueConservationFuzz, PerpCollateralConserved)
{
  g_failures = 0;  // independent baseline: a prior scenario must not taint this one
  test_perp_conservation(/*adl=*/false);
  EXPECT_EQ(g_failures, 0);
}

TEST(VenueConservationFuzz, PerpWithAdlConserved)
{
  g_failures = 0;  // independent baseline: a prior scenario must not taint this one
  test_perp_conservation(/*adl=*/true);
  EXPECT_EQ(g_failures, 0);
}

TEST(VenueConservationFuzz, CrossMarginConserved)
{
  g_failures = 0;  // independent baseline: a prior scenario must not taint this one
  test_cross_margin_conservation();
  EXPECT_EQ(g_failures, 0);
}

TEST(VenueConservationFuzz, MultiCollateralConserved)
{
  g_failures = 0;  // independent baseline: a prior scenario must not taint this one
  test_multi_collateral_conservation();
  EXPECT_EQ(g_failures, 0);
}
