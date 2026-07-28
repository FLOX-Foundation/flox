/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/cross_margin.h"
#include "flox-venue/index_feed.h"
#include "flox-venue/ledger.h"
#include "flox-venue/mark_feed_driver.h"

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

constexpr AssetId USD = 1;
constexpr uint64_t VENUE = 999;
constexpr SymbolId BTC = 1;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount usd(double v) { return amountOf(Volume::fromDouble(v)); }
constexpr int64_t SEC = 1'000'000'000LL;

void feedSources(IndexAggregator& idx, double p, int64_t now)
{
  idx.update(1, px(p), now);
  idx.update(2, px(p + 0.01), now);
  idx.update(3, px(p - 0.01), now);
}

void test_driver()
{
  std::printf("test_mark_feed_driver\n");
  Ledger led;
  led.deposit(1, USD, usd(1000));
  led.deposit(2, USD, usd(100000));
  std::vector<Liquidation> liqs;
  CrossMarginManager cm(led, USD, VENUE, [&](const Liquidation& l)
                        { liqs.push_back(l); });
  cm.configureSymbol(BTC, 1000, 500);
  cm.setMark(BTC, px(100));
  cm.applyFill(1, BTC, Side::BUY, qty(100).raw(), px(100).raw());
  cm.applyFill(2, BTC, Side::SELL, qty(100).raw(), px(100).raw());

  IndexAggregator idx(/*staleness*/ 5 * SEC, /*maxDevBps*/ 200, /*minSources*/ 3);
  MarkPrice mark(/*clampBps*/ 500);
  MarkFeedDriver<CrossMarginManager> drv(idx, mark, cm, BTC, 500);

  // Fresh feed at 100 -> mark published, not paused, no liquidation.
  feedSources(idx, 100.0, 1 * SEC);
  CHECK(drv.onTick(1 * SEC, px(100).raw(), px(100).raw()));
  CHECK(!drv.paused());
  CHECK(liqs.empty());
  CHECK(drv.markAgeNs(1 * SEC) == 0);                          // just published
  CHECK(drv.markAgeNs(1 * SEC + 500'000'000) == 500'000'000);  // 500ms later

  // Feed goes stale: no source refresh, and time advances past the TTL. A crash
  // would liquidate, but the driver freezes liquidations instead.
  const int64_t t = 10 * SEC;  // last sources were at 1s, TTL 5s -> stale
  const bool published = drv.onTick(t, px(80).raw(), px(80).raw());
  CHECK(!published);
  CHECK(drv.paused());
  CHECK(liqs.empty());                              // NOT liquidated on a stale feed
  CHECK(cm.positionQty(1, BTC) == qty(100).raw());  // still open

  // Feed recovers at the real (crashed) price -> resume and liquidate.
  feedSources(idx, 80.0, 11 * SEC);
  CHECK(drv.onTick(11 * SEC, px(80).raw(), px(80).raw()));
  CHECK(!drv.paused());
  CHECK(!liqs.empty());
  CHECK(cm.positionQty(1, BTC) == 0);
}

}  // namespace

TEST(MarkFeedDriver, EngineSuite)
{
  test_driver();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
