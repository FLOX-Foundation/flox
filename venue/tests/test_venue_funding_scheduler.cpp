/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/funding_scheduler.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include <gtest/gtest.h>

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
constexpr uint64_t VENUE = 999;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }
constexpr int64_t SEC = 1'000'000'000LL;

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;
  return c;
}
NewOrder ord(OrderId id, Side s, double p, double q, uint64_t acct)
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

void test_scheduler()
{
  std::printf("test_funding_scheduler\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(1000));
  led.deposit(2, QUOTE, quote(1000));
  MatchingEngine<MatchingBook> eng(cfg(), [](const OutboundEvent&) {});
  eng.setLedger(&led, VENUE);
  eng.submit(InboundCommand{ord(1, Side::BUY, 100, 10, 1)}, 0);   // long 10 @ 100
  eng.submit(InboundCommand{ord(2, Side::SELL, 100, 10, 2)}, 1);  // short 10 @ 100

  // Funding interval 8h (in ns here just a window); sample every "hour".
  const int64_t interval = 8 * SEC;
  FundingScheduler<MatchingEngine<MatchingBook>> sched(eng, interval);

  // Perp trades at a persistent 0.5% premium over the index across the interval.
  std::optional<double> settled;
  for (int h = 0; h <= 8; ++h)
  {
    auto r = sched.onTick(h * SEC, /*mark*/ px(100.5), /*index*/ px(100.0));
    if (r)
    {
      settled = r;
    }
  }

  CHECK(sched.settlements() == 1);
  CHECK(settled.has_value());
  CHECK(*settled > 0.0);  // positive premium -> positive funding -> longs pay

  // Long (acct1) paid, short (acct2) received; pool nets zero.
  CHECK(led.available(1, QUOTE) < quote(900));  // < 900 (margin 100 reserved + paid funding)
  CHECK(led.available(2, QUOTE) > quote(900));
  CHECK(led.total(VENUE, QUOTE) == 0);

  // A second interval with mark == index settles ~the interest floor, and the
  // accumulator reset means the prior 0.5% premium does not carry over.
  for (int h = 9; h <= 16; ++h)
  {
    sched.onTick(h * SEC, px(100.0), px(100.0));
  }
  CHECK(sched.settlements() == 2);
  CHECK(sched.lastRate() < *settled);  // second rate near the floor, below the first
}

}  // namespace

TEST(FundingScheduler, EngineSuite)
{
  test_scheduler();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
