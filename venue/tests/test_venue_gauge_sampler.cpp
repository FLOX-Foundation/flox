/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/cross_margin.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/metrics.h"
#include "flox-venue/prometheus.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>

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
bool contains(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

constexpr AssetId USD = 1;
constexpr uint64_t VENUE = 999;
constexpr SymbolId SPOT_SYM = 5;
constexpr AssetId SPOT_BASE = 2;
constexpr SymbolId PERP = 1;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount usd(double v) { return amountOf(Volume::fromDouble(v)); }

// Sample live venue state into the observability Gauges struct.
Gauges sampleGauges(const Ledger& led, const CrossMarginManager& cm,
                    const MatchingEngine<MatchingBook>& spot, double lastFundingRate)
{
  Gauges g;
  g.insuranceFundRaw = led.total(VENUE, USD);
  g.openInterestRaw = cm.openInterestRaw();
  g.openPositions = cm.openPositionCount();
  g.restingOrders = spot.restingOrderCount();
  g.fundingRate = lastFundingRate;
  return g;
}

void test_sampler()
{
  std::printf("test_gauge_sampler\n");
  Ledger led;
  led.deposit(VENUE, USD, usd(100000));  // seed insurance fund
  led.deposit(1, USD, usd(100000));
  led.deposit(2, USD, usd(100000));
  led.deposit(1, SPOT_BASE, amountOf(qty(1000)));

  // Cross-margin perp positions -> open interest.
  CrossMarginManager cm(led, USD, VENUE);
  cm.configureSymbol(PERP, 1000, 500);
  cm.setMark(PERP, px(100));
  cm.applyFill(1, PERP, Side::BUY, qty(30).raw(), px(100).raw());
  cm.applyFill(2, PERP, Side::SELL, qty(30).raw(), px(100).raw());
  CHECK(cm.openPositionCount() == 2);
  CHECK(cm.openInterestRaw() == usd(6000));  // 2 * 30 * 100

  // A spot engine with two resting orders.
  SymbolConfig sc;
  sc.id = SPOT_SYM;
  sc.tickSize = px(0.01);
  sc.baseAsset = SPOT_BASE;
  sc.quoteAsset = USD;
  MatchingEngine<MatchingBook> spot(sc, [](const OutboundEvent&) {});
  spot.setLedger(&led, VENUE);
  NewOrder a;
  a.id = 1;
  a.symbol = SPOT_SYM;
  a.side = Side::SELL;
  a.type = OrderType::LIMIT;
  a.price = px(101);
  a.quantity = qty(3);
  a.accountId = 1;
  spot.submit(InboundCommand{a}, 0);
  NewOrder b = a;
  b.id = 2;
  b.price = px(102);
  spot.submit(InboundCommand{b}, 1);
  CHECK(spot.restingOrderCount() == 2);

  const Gauges g = sampleGauges(led, cm, spot, /*fundingRate*/ 0.00013);
  Metrics metrics;
  const std::string page = prom::render(metrics, g);

  CHECK(contains(page, "fme_insurance_fund_raw 10000000000000"));  // 100000 * 1e8
  CHECK(contains(page, "fme_open_interest_raw 600000000000"));     // 6000 * 1e8
  CHECK(contains(page, "fme_open_positions 2"));
  CHECK(contains(page, "fme_resting_orders 2"));
  CHECK(contains(page, "fme_funding_rate"));
}

}  // namespace

TEST(GaugeSampler, EngineSuite)
{
  test_sampler();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
