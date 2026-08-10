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

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
constexpr int64_t SEC = 1'000'000'000LL;

void test_index_median()
{
  std::printf("test_index_median\n");
  IndexAggregator idx(/*staleness*/ 5 * SEC, /*maxDevBps*/ 100, /*minSources*/ 3);

  idx.update(1, px(100.0), 1 * SEC);
  idx.update(2, px(100.2), 1 * SEC);
  CHECK(!idx.hasIndex(1 * SEC));  // only 2 sources < min 3

  idx.update(3, px(99.8), 1 * SEC);
  CHECK(idx.hasIndex(1 * SEC));
  CHECK(idx.index(1 * SEC) == px(100.0));  // median of {99.8, 100.0, 100.2}
}

void test_stale_source_ignored()
{
  std::printf("test_stale_source_ignored\n");
  IndexAggregator idx(5 * SEC, 100, 3);
  idx.update(1, px(100.0), 1 * SEC);
  idx.update(2, px(100.0), 1 * SEC);
  idx.update(3, px(100.0), 1 * SEC);
  idx.update(4, px(500.0), 1 * SEC);  // 4th source, wildly off but...

  // At t = 8s, sources from t=1s are stale (>5s). All four stale -> no index.
  CHECK(!idx.hasIndex(8 * SEC));
  // Refresh three good sources at t=8s; the bad one stays stale and is dropped.
  idx.update(1, px(101.0), 8 * SEC);
  idx.update(2, px(101.0), 8 * SEC);
  idx.update(3, px(101.0), 8 * SEC);
  CHECK(idx.hasIndex(8 * SEC));
  CHECK(idx.index(8 * SEC) == px(101.0));  // stale 500.0 ignored
}

void test_outlier_rejected()
{
  std::printf("test_outlier_rejected\n");
  IndexAggregator idx(5 * SEC, /*maxDevBps*/ 100 /*=1%*/, 3);
  idx.update(1, px(100.0), 1 * SEC);
  idx.update(2, px(100.0), 1 * SEC);
  idx.update(3, px(100.1), 1 * SEC);
  idx.update(4, px(150.0), 1 * SEC);  // fresh but +50% -> gross outlier

  // Median of all four raw would be pulled up; outlier filter drops 150.0.
  CHECK(idx.index(1 * SEC) == px(100.0));
}

void test_broad_move_tracked()
{
  std::printf("test_broad_move_tracked\n");
  IndexAggregator idx(5 * SEC, 100, 3);
  // All sources move together by ~5% -> this is a real move, not an outlier.
  idx.update(1, px(105.0), 1 * SEC);
  idx.update(2, px(105.1), 1 * SEC);
  idx.update(3, px(104.9), 1 * SEC);
  CHECK(idx.index(1 * SEC) == px(105.0));  // tracked, not suppressed
}

void test_mark_clamp()
{
  std::printf("test_mark_clamp\n");
  MarkPrice mark(/*clampBps*/ 50);  // 0.5% band
  CHECK(!mark.valid());

  mark.setIndex(px(100.0));
  CHECK(mark.valid());
  CHECK(mark.value() == px(100.0));  // only index -> mark = index

  // A modest last print within band + mid: median of {100, 100.1, 100.05}.
  mark.setLast(px(100.1));
  mark.setMid(px(100.05));
  CHECK(mark.value() == px(100.05));

  // A manipulative print far above: mark clamps to index + 0.5% = 100.5.
  mark.setLast(px(120.0));
  mark.setMid(px(120.0));
  CHECK(mark.value() == px(100.5));

  // And far below clamps to index - 0.5% = 99.5.
  mark.setLast(px(1.0));
  mark.setMid(px(1.0));
  CHECK(mark.value() == px(99.5));
}

void test_impact_price()
{
  std::printf("test_impact_price\n");
  // A spoofy dust ask at 90, real depth at 100. Naive best ask = 90; the impact
  // price to fill a real size walks past the dust to the true ~100.
  std::vector<DepthLevel> asks = {{px(90).raw(), qty(0.01).raw()}, {px(100).raw(), qty(100).raw()}};
  const int64_t ip = impactPriceRaw(asks, qty(10).raw());
  CHECK(Price::fromRaw(ip) > px(99));  // not dragged down to the 90 dust
  CHECK(Price::fromRaw(ip) < px(100.1));

  // Symmetric real depth -> impact mid exactly at 100.
  std::vector<DepthLevel> bids = {{px(99.98).raw(), qty(1).raw()}, {px(99.90).raw(), qty(100).raw()}};
  std::vector<DepthLevel> asks2 = {{px(100.02).raw(), qty(1).raw()}, {px(100.10).raw(), qty(100).raw()}};
  CHECK(Price::fromRaw(impactMidRaw(bids, asks2, qty(10).raw())) == px(100.0));

  // Insufficient depth -> VWAP of what's available; empty -> 0 (no crash).
  std::vector<DepthLevel> thin = {{px(100).raw(), qty(2).raw()}};
  CHECK(impactPriceRaw(thin, qty(10).raw()) == px(100).raw());
  CHECK(impactPriceRaw({}, qty(10).raw()) == 0);

  // Impact mid feeds the mark: a thin manipulated top can't move it.
  MarkPrice mark(/*clampBps*/ 200);
  mark.setIndex(px(100));
  mark.setMid(Price::fromRaw(impactMidRaw(bids, asks2, qty(10).raw())));
  CHECK(mark.value() == px(100.0));
}

// The impact-mid adapter runs on a real engine book, not synthetic levels.
void test_book_impact_mid()
{
  std::printf("test_book_impact_mid\n");
  SymbolConfig sc;
  sc.id = 1;
  sc.tickSize = px(0.01);
  sc.minPrice = px(50);
  sc.maxPrice = px(150);
  MatchingEngine<MatchingBook> eng(sc, [](const OutboundEvent&) {});

  auto rest = [&](OrderId id, Side s, double p, double q)
  {
    NewOrder o;
    o.id = id;
    o.symbol = 1;
    o.side = s;
    o.type = OrderType::LIMIT;
    o.price = px(p);
    o.quantity = qty(q);
    o.accountId = 1;
    eng.submit(InboundCommand{o}, 0);
  };
  // Thin top-of-book (1 lot each side) over real depth further out.
  rest(1, Side::BUY, 99.98, 1);
  rest(2, Side::BUY, 99.90, 100);
  rest(3, Side::SELL, 100.02, 1);
  rest(4, Side::SELL, 100.10, 100);

  // Impact mid for a 10-lot walks past the thin top into the real depth and
  // lands symmetrically at 100; a raw top-of-book mid would also be ~100 here,
  // but the impact mid is what resists a one-lot top-of-book manipulation.
  const int64_t mid = bookImpactMidRaw(eng.book(), qty(10).raw());
  CHECK(Price::fromRaw(mid) == px(100.0));
  // With a 1-lot impact size it only sees the thin top: still 100 here.
  CHECK(Price::fromRaw(bookImpactMidRaw(eng.book(), qty(1).raw())) == px(100.0));
}

// End-to-end: a manipulative perp print must not liquidate a healthy account,
// because the mark fed to the risk engine is clamped to the index band.
void test_clamp_prevents_liquidation()
{
  std::printf("test_clamp_prevents_liquidation\n");
  constexpr AssetId USD = 1;
  constexpr uint64_t VENUE = 999;
  constexpr SymbolId BTC = 1;

  Ledger led;
  led.deposit(1, USD, amountOf(Volume::fromDouble(1000)));
  led.deposit(2, USD, amountOf(Volume::fromDouble(100000)));

  std::vector<Liquidation> liqs;
  CrossMarginManager risk(led, USD, VENUE, [&](const Liquidation& l)
                          { liqs.push_back(l); });
  risk.configureSymbol(BTC, /*im*/ 1000, /*mm*/ 500);
  risk.setMark(BTC, px(100));
  // Account 1 long 100 @ 100: notional 10000, IM 1000 = equity, MM 500.
  risk.applyFill(1, BTC, Side::BUY, Quantity::fromDouble(100).raw(), px(100).raw());
  risk.applyFill(2, BTC, Side::SELL, Quantity::fromDouble(100).raw(), px(100).raw());

  // Index (external spot) is steady at 100; an attacker slams the thin perp book
  // to 80. Raw 80 would give equity 1000 + (80-100)*100 = -1000 -> liquidation.
  IndexAggregator idx(5 * SEC, 100, 3);
  idx.update(1, px(100.0), 1 * SEC);
  idx.update(2, px(100.0), 1 * SEC);
  idx.update(3, px(100.0), 1 * SEC);

  MarkPrice mark(/*clampBps*/ 200);  // 2% band
  mark.setIndex(idx.index(1 * SEC));
  mark.setLast(px(80));  // the manipulated print
  mark.setMid(px(80));

  const Price clamped = mark.value();
  CHECK(clamped == px(98));  // clamped to index - 2%
  risk.setMark(BTC, clamped);

  // equity = 1000 + (98-100)*100 = 800; MM = 98*100*5% = 490 -> solvent, no liq.
  CHECK(liqs.empty());
  CHECK(risk.positionQty(1, BTC) == Quantity::fromDouble(100).raw());
}

}  // namespace

TEST(MarkPrice, EngineSuite)
{
  test_index_median();
  test_stale_source_ignored();
  test_outlier_rejected();
  test_broad_move_tracked();
  test_mark_clamp();
  test_impact_price();
  test_book_impact_mid();
  test_clamp_prevents_liquidation();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
