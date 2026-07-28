/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/cross_margin.h"
#include "flox-venue/funding_scheduler.h"
#include "flox-venue/index_feed.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/metrics.h"
#include "flox-venue/prometheus.h"
#include "flox/book/matching_book.h"

#include <gtest/gtest.h>
#include <cstdint>

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
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE = 777;
constexpr int NACCT = 5;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }
constexpr int64_t MS = 1'000'000LL;

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

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50);
  c.maxPrice = px(150);
  c.quoteAsset = QUOTE;
  return c;  // pure matcher: no linearPerp, no ledger -> engine just crosses
}

void test_perp_venue()
{
  std::printf("test_perp_venue\n");
  Ledger led;
  led.deposit(VENUE, QUOTE, quote(100000));  // seed insurance fund
  for (int a = 1; a <= NACCT; ++a)
  {
    led.deposit(a, QUOTE, quote(100000));
  }
  const Amount init = [&]
  {
    Amount t = led.total(VENUE, QUOTE);
    for (int a = 1; a <= NACCT; ++a)
    {
      t += led.total(a, QUOTE);
    }
    return t;
  }();

  // The money + risk authority (cross-margin), fed from the trade stream.
  CrossMarginManager cm(led, QUOTE, VENUE, [](const Liquidation&) {}, /*autoDeleverage*/ true);
  cm.configureSymbol(SYM, /*im*/ 1000, /*mm*/ 500);
  cm.setMark(SYM, px(100));

  Metrics metrics;
  int64_t lastTradeRaw = px(100).raw();

  // Engine as a pure matcher: on each Trade, settle BOTH sides into cross-margin.
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     metrics.observe(e);
                                     if (const auto* t = std::get_if<Trade>(&e))
                                     {
                                       const Side takerSide = t->takerSide;
                                       const Side makerSide =
                                           takerSide == Side::BUY ? Side::SELL : Side::BUY;
                                       cm.applyFill(t->takerAccount, SYM, takerSide, t->quantity.raw(),
                                                    t->price.raw());
                                       cm.applyFill(t->makerAccount, SYM, makerSide, t->quantity.raw(),
                                                    t->price.raw());
                                       lastTradeRaw = t->price.raw();
                                     } });

  IndexAggregator idx(/*staleness*/ 60 * MS, /*maxDevBps*/ 200, /*minSources*/ 3);
  MarkPrice mark(/*clampBps*/ 300);
  FundingScheduler<MatchingEngine<MatchingBook>> sched(eng, /*interval*/ 1000 * MS);

  auto quoteConserved = [&]
  {
    Amount t = led.total(VENUE, QUOTE);
    for (int a = 1; a <= NACCT; ++a)
    {
      t += led.total(a, QUOTE);
    }
    return t == init;
  };

  Rng rng{0xBADC0FFEEULL};
  const int64_t midRaw = px(100).raw();
  const int64_t tickRaw = px(0.01).raw();
  OrderId nextId = 1;
  int breaches = 0;
  int64_t refRaw = midRaw;  // external index reference (random walk)

  const int OPS = 60000;
  for (int i = 0; i < OPS; ++i)
  {
    const int64_t now = static_cast<int64_t>(i) * MS;
    const uint64_t r = rng.next();

    // External index random-walks; refresh three spot sources (kept fresh).
    refRaw += (static_cast<int64_t>((r >> 40) % 3) - 1) * tickRaw;
    if (refRaw < px(80).raw())
    {
      refRaw = px(80).raw();
    }
    if (refRaw > px(120).raw())
    {
      refRaw = px(120).raw();
    }
    idx.update(1, Price::fromRaw(refRaw), now);
    idx.update(2, Price::fromRaw(refRaw + tickRaw), now);
    idx.update(3, Price::fromRaw(refRaw - tickRaw), now);

    const uint32_t kind = r % 100;
    if (kind < 70)
    {
      NewOrder o;
      o.id = nextId++;
      o.symbol = SYM;
      o.side = (r & 1) ? Side::BUY : Side::SELL;
      o.accountId = 1 + (r >> 8) % NACCT;
      const int ticks = static_cast<int>((r >> 1) % 41) - 20;
      o.price = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
      o.quantity = qty(1.0 + static_cast<double>((r >> 20) % 4));
      o.type = OrderType::LIMIT;
      // Pre-trade portfolio buying-power gate (ingress credit check).
      if (cm.canOpen(o.accountId, SYM, o.side, o.quantity.raw(), o.price.raw()))
      {
        eng.submit(InboundCommand{o}, now);
      }
    }

    // Mark: median(index, impact-mid from the live book, last trade), clamped.
    if (idx.hasIndex(now))
    {
      mark.setIndex(idx.index(now));
      const int64_t impact = bookImpactMidRaw(eng.book(), qty(3).raw());
      if (impact > 0)
      {
        mark.setMid(Price::fromRaw(impact));
      }
      mark.setLast(Price::fromRaw(lastTradeRaw));
      const Price mk = mark.value();
      cm.setMark(SYM, mk);                    // may liquidate underwater accounts
      sched.onTick(now, mk, idx.index(now));  // may settle funding
    }

    if (!quoteConserved())
    {
      ++breaches;
      if (breaches == 1)
      {
        std::printf("  first breach at op %d\n", i);
      }
    }
  }

  CHECK(breaches == 0);
  CHECK(metrics.trades > 0);

  // Final observability snapshot from live venue state.
  Gauges g;
  g.insuranceFundRaw = led.total(VENUE, QUOTE);
  g.openInterestRaw = cm.openInterestRaw();
  g.openPositions = cm.openPositionCount();
  g.restingOrders = eng.restingOrderCount();
  g.fundingRate = sched.lastRate();
  const std::string page = prom::render(metrics, g);
  CHECK(page.find("fme_open_interest_raw") != std::string::npos);
  CHECK(page.find("fme_insurance_fund_raw") != std::string::npos);

  std::printf("  %d ops: trades=%llu, funding settlements=%llu, OI=%.2f, insurance=%.2f, breaches=%d\n",
              OPS, (unsigned long long)metrics.trades, (unsigned long long)sched.settlements(),
              (double)cm.openInterestRaw() / 1e8, (double)led.total(VENUE, QUOTE) / 1e8, breaches);
}

}  // namespace

TEST(PerpVenue, EngineSuite)
{
  test_perp_venue();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
