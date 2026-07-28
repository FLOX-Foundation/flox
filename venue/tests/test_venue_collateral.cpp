/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/collateral.h"
#include "flox-venue/ledger.h"

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

constexpr AssetId USD = 1;
constexpr AssetId BTC = 2;
constexpr AssetId ETH = 3;
constexpr AssetId DOGE = 4;  // not accepted as collateral
Price px(double v) { return Price::fromDouble(v); }
Amount units(double v) { return amountOf(Volume::fromDouble(v)); }
Amount usd(double v) { return amountOf(Volume::fromDouble(v)); }

void test_collateral()
{
  std::printf("test_collateral\n");
  CollateralSchedule sched;
  sched.configure(USD, px(1.0).raw(), /*haircut*/ 0);       // quote asset: par, no haircut
  sched.configure(BTC, px(30000).raw(), /*haircut*/ 2000);  // 20% haircut
  sched.configure(ETH, px(2000).raw(), /*haircut*/ 1000);   // 10% haircut

  CHECK(sched.accepts(USD) && sched.accepts(BTC) && !sched.accepts(DOGE));

  // Per-asset haircut-adjusted values.
  CHECK(sched.value(USD, usd(1000)) == usd(1000));    // par
  CHECK(sched.value(BTC, units(0.5)) == usd(12000));  // 0.5 * 30000 * 0.8
  CHECK(sched.value(ETH, units(10)) == usd(18000));   // 10 * 2000 * 0.9
  CHECK(sched.value(DOGE, units(100000)) == 0);       // not accepted -> 0 credit

  // Whole-basket portfolio value from the ledger.
  Ledger led;
  led.deposit(1, USD, usd(1000));
  led.deposit(1, BTC, units(0.5));
  led.deposit(1, ETH, units(10));
  led.deposit(1, DOGE, units(100000));                                         // ignored
  CHECK(sched.portfolioValue(led, 1) == usd(1000) + usd(12000) + usd(18000));  // 31000

  // Reserved balances still count (collateral is locked but owned).
  led.reserve(1, USD, usd(400));
  CHECK(sched.portfolioValue(led, 1) == usd(31000));

  // A BTC price drop re-values the basket.
  sched.setPrice(BTC, px(20000).raw());
  CHECK(sched.value(BTC, units(0.5)) == usd(8000));                           // 0.5 * 20000 * 0.8
  CHECK(sched.portfolioValue(led, 1) == usd(1000) + usd(8000) + usd(18000));  // 27000
}

}  // namespace

TEST(Collateral, EngineSuite)
{
  test_collateral();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
