/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Clearing invariants: what portfolio margin must refuse, what a liquidation
// may pay out, and what a haircut is allowed to touch.
//
// The cases here assert money properties rather than code paths -- nobody's
// balance grows out of nothing, the insurance fund is not the counterparty of
// last resort for a position the venue never risk-configured, and a debt is
// worth its full size. The existing cross-margin and collateral suites cover
// the configured, marked, positive-balance happy path; these cover the states
// that path leaves out.

#include "flox-venue/collateral.h"
#include "flox-venue/cross_margin.h"
#include "flox-venue/ledger.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr AssetId USD = 1;
constexpr AssetId BTC_ASSET = 2;
constexpr uint64_t VENUE = 999;
constexpr SymbolId MARKED = 1;    // symbol that has a mark price
constexpr SymbolId UNMARKED = 2;  // symbol configured for risk, never marked

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount usd(double v) { return amountOf(Volume::fromDouble(v)); }

// Cross a fill through the manager for both sides so the clearing pool stays
// balanced, exactly as two symbol shards report their two sides.
void crossFill(CrossMarginManager& m, SymbolId s, uint64_t buyer, uint64_t seller, double p,
               double q)
{
  m.applyFill(buyer, s, Side::BUY, qty(q).raw(), px(p).raw());
  m.applyFill(seller, s, Side::SELL, qty(q).raw(), px(p).raw());
}

}  // namespace

// An auto-deleverage sweep ranks and closes winners at the mark. On a symbol
// that never received one, the fallback used to be a literal zero, so a short
// leg looked like it had earned its entire notional and the fund paid for the
// invention. Everything else in the file already falls back to the leg's entry
// price, which prices the leg at no profit and no loss.
TEST(VenueClearingIntegrity, AutoDeleverageOnAnUnmarkedSymbolPaysNoPhantomProfit)
{
  Ledger led;
  led.deposit(1, USD, usd(50));      // about to go bankrupt
  led.deposit(2, USD, usd(100000));  // its counterparty on the marked symbol
  led.deposit(3, USD, usd(1000));    // short the unmarked symbol: the ADL target

  std::vector<Liquidation> tape;
  CrossMarginManager m(
      led, USD, VENUE, [&tape](const Liquidation& l)
      { tape.push_back(l); },
      /*autoDeleverage*/ true);
  m.configureSymbol(MARKED, /*im*/ 1000, /*mm*/ 500);
  m.configureSymbol(UNMARKED, /*im*/ 1000, /*mm*/ 500);
  m.setMark(MARKED, px(100));
  // UNMARKED deliberately never marked: a listed instrument whose mark feed has
  // not published yet is a documented state.

  crossFill(m, UNMARKED, /*buyer*/ 1, /*seller*/ 3, 100, 1);
  crossFill(m, MARKED, /*buyer*/ 1, /*seller*/ 2, 100, 1);

  const Amount venueBefore = led.available(VENUE, USD);
  const Amount winnerBefore = led.available(3, USD);

  m.setMark(MARKED, px(20));  // account 1 loses 80 against 50 of collateral

  EXPECT_EQ(led.available(3, USD), winnerBefore)
      << "a winner on an unmarked symbol must not be paid for a price that does not exist";
  EXPECT_EQ(m.positionQty(3, UNMARKED), qty(1).raw() * -1)
      << "and its position must survive: there was no deficit to recover from it";
  // The deficit is recovered from the real winner -- the short on the marked
  // symbol -- so the clearing pool comes out square rather than short the
  // notional of a leg nobody could price.
  EXPECT_EQ(led.available(VENUE, USD), venueBefore)
      << "the insurance fund absorbs a bankruptcy, it does not fund a windfall";

  for (const Liquidation& l : tape)
  {
    if (l.symbol == UNMARKED)
    {
      EXPECT_EQ(l.price, px(100))
          << "the public tape must not print a zero price for a leg closed at its entry";
    }
  }
}

// A symbol with no risk profile used to require no margin at all, which is an
// infinite leverage grant rather than a conservative default.
TEST(VenueClearingIntegrity, UnconfiguredSymbolCannotBeOpened)
{
  Ledger led;
  led.deposit(1, USD, usd(1000));
  CrossMarginManager m(led, USD, VENUE);
  m.configureSymbol(MARKED, /*im*/ 1000, /*mm*/ 500);
  m.setMark(MARKED, px(100));

  constexpr SymbolId UNCONFIGURED = 7;
  EXPECT_FALSE(m.canOpen(1, UNCONFIGURED, Side::BUY, qty(2000000).raw(), px(100).raw()))
      << "200000x leverage on a symbol the risk config has never seen";
  EXPECT_FALSE(m.canOpen(1, UNCONFIGURED, Side::BUY, qty(1).raw(), px(100).raw()))
      << "size is not the point: the symbol carries no margin rule at all";

  // The configured symbol is unaffected: 1000 of equity at 10% IM carries
  // 10000 of notional and no more.
  EXPECT_TRUE(m.canOpen(1, MARKED, Side::BUY, qty(100).raw(), px(100).raw()));
  EXPECT_FALSE(m.canOpen(1, MARKED, Side::BUY, qty(101).raw(), px(100).raw()));
}

// If such a leg exists anyway (a fill reported before the symbol was
// configured), it must still carry a margin requirement, or the account is
// only liquidated once its collateral is entirely gone -- by which point the
// hole is far larger than the collateral that was supposed to cover it.
TEST(VenueClearingIntegrity, UnconfiguredLegIsMarginedBeforeItsCollateralIsGone)
{
  Ledger led;
  led.deposit(1, USD, usd(1000));
  led.deposit(2, USD, usd(100000000));

  CrossMarginManager m(led, USD, VENUE);
  constexpr SymbolId UNCONFIGURED = 7;
  m.setMark(UNCONFIGURED, px(100));
  crossFill(m, UNCONFIGURED, /*buyer*/ 1, /*seller*/ 2, 100, 20000);  // 2,000,000 notional

  EXPECT_GT(m.maintenanceMargin(1), usd(0))
      << "a leg the venue cannot price the risk of must be treated as maximal risk";

  m.setMark(UNCONFIGURED, px(100));  // the same price: no move at all
  EXPECT_EQ(m.positionQty(1, UNCONFIGURED), 0)
      << "the breach is visible at the entry price, before any adverse move";

  m.setMark(UNCONFIGURED, px(99.5));  // half a percent against account 1
  EXPECT_EQ(led.available(1, USD), usd(1000))
      << "a position closed while it was still whole cannot lose the account its collateral";
}

// A haircut discounts what an account owns. Applying it to what an account owes
// discounts the debt, which shows up directly as equity that is not there.
TEST(VenueClearingIntegrity, HaircutDoesNotDiscountADebt)
{
  CollateralSchedule sched;
  sched.configure(BTC_ASSET, px(100000).raw(), /*haircut*/ 2000);  // 20%

  EXPECT_EQ(sched.value(BTC_ASSET, amountOf(Quantity::fromDouble(1.0))), usd(80000))
      << "one coin owned is worth its haircut value";
  EXPECT_EQ(sched.value(BTC_ASSET, -amountOf(Quantity::fromDouble(1.0))), usd(-100000))
      << "one coin owed is a liability of its full size";
}

TEST(VenueClearingIntegrity, DiscountedDebtCannotBeWithdrawn)
{
  Ledger led;
  led.deposit(1, USD, usd(100000));
  led.deposit(1, BTC_ASSET, -amountOf(Quantity::fromDouble(1.0)));  // funding drove it negative

  CollateralSchedule sched;
  sched.configure(USD, px(1).raw(), 0);
  sched.configure(BTC_ASSET, px(100000).raw(), 2000);

  CrossMarginManager m(led, USD, VENUE);
  m.setCollateralSchedule(&sched);

  EXPECT_EQ(m.equity(1), usd(0)) << "100000 of quote against 100000 of debt is no equity at all";
  EXPECT_EQ(m.withdrawable(1), usd(0));
  EXPECT_FALSE(m.canWithdraw(1, usd(20000)));
}
