/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/collateral.h"
#include "flox-venue/cross_margin.h"
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
constexpr uint64_t VENUE = 999;
constexpr SymbolId BTC = 1;
constexpr SymbolId ETH = 2;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount usd(double v) { return amountOf(Volume::fromDouble(v)); }

// Cross a fill through the manager for BOTH counterparties so the clearing pool
// stays balanced (every long has a short), exactly as two symbol shards would
// report their two sides to the risk consumer.
void crossFill(CrossMarginManager& m, SymbolId s, uint64_t buyer, uint64_t seller, double p, double q)
{
  m.applyFill(buyer, s, Side::BUY, qty(q).raw(), px(p).raw());
  m.applyFill(seller, s, Side::SELL, qty(q).raw(), px(p).raw());
}

void test_portfolio_im_gate()
{
  std::printf("test_cross_margin_im_gate\n");
  Ledger led;
  led.deposit(1, USD, usd(1000));
  CrossMarginManager m(led, USD, VENUE);
  m.configureSymbol(BTC, /*im*/ 1000, /*mm*/ 500);  // 10% IM
  m.configureSymbol(ETH, /*im*/ 1000, /*mm*/ 500);
  m.setMark(BTC, px(100));
  m.setMark(ETH, px(100));

  // 1000 equity, 10% IM -> can carry 10000 notional total.
  CHECK(m.canOpen(1, BTC, Side::BUY, qty(50).raw(), px(100).raw()));  // 5000 notional -> IM 500 <= 1000
  crossFill(m, BTC, 1, 2, 100, 50);
  CHECK(m.initialMargin(1) == usd(500));

  // Add ETH 40@100 = 4000 notional -> total IM 900 <= 1000: allowed.
  CHECK(m.canOpen(1, ETH, Side::BUY, qty(40).raw(), px(100).raw()));
  crossFill(m, ETH, 1, 2, 100, 40);
  CHECK(m.initialMargin(1) == usd(900));

  // One more big BTC clip would push IM past equity -> rejected.
  CHECK(!m.canOpen(1, BTC, Side::BUY, qty(30).raw(), px(100).raw()));  // +3000 -> IM 1200 > 1000
}

void test_cross_offset()
{
  std::printf("test_cross_margin_offset\n");
  Ledger led;
  led.deposit(1, USD, usd(1000));
  led.deposit(2, USD, usd(100000));  // deep counterparty
  const Amount init = led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD);

  CrossMarginManager m(led, USD, VENUE);
  m.configureSymbol(BTC, 1000, 500);
  m.configureSymbol(ETH, 1000, 500);
  m.setMark(BTC, px(100));
  m.setMark(ETH, px(100));

  // Account 1: long BTC 20, short ETH 20 (both entered at 100).
  crossFill(m, BTC, /*buyer*/ 1, /*seller*/ 2, 100, 20);
  crossFill(m, ETH, /*buyer*/ 2, /*seller*/ 1, 100, 20);
  CHECK(m.positionQty(1, BTC) == qty(20).raw());
  CHECK(m.positionQty(1, ETH) == -qty(20).raw());

  // BTC +10 (long gains 200), ETH +10 (short loses 200): equity unchanged.
  m.setMark(BTC, px(110));
  m.setMark(ETH, px(110));
  CHECK(m.equity(1) == usd(1000));  // +200 and -200 net to zero across the portfolio
  // No liquidation happened (positions still open).
  CHECK(m.positionQty(1, BTC) == qty(20).raw());
  CHECK(m.positionQty(1, ETH) == -qty(20).raw());

  // Close both at the marks -> realized PnL nets to zero, wallet back to 1000.
  crossFill(m, BTC, /*buyer*/ 2, /*seller*/ 1, 110, 20);
  crossFill(m, ETH, /*buyer*/ 1, /*seller*/ 2, 110, 20);
  CHECK(m.positionQty(1, BTC) == 0 && m.positionQty(1, ETH) == 0);
  CHECK(led.available(1, USD) == usd(1000));
  CHECK(led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD) == init);
}

void test_cross_liquidation()
{
  std::printf("test_cross_margin_liquidation\n");
  Ledger led;
  led.deposit(1, USD, usd(1000));
  led.deposit(2, USD, usd(100000));
  const Amount init = led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD);

  std::vector<Liquidation> liqs;
  CrossMarginManager m(led, USD, VENUE, [&](const Liquidation& l)
                       { liqs.push_back(l); });
  m.configureSymbol(BTC, 1000, 500);
  m.configureSymbol(ETH, 1000, 500);
  m.setMark(BTC, px(100));
  m.setMark(ETH, px(100));

  // Account 1 goes long BTC 50 and long ETH 50 (both directions same-way -> no
  // internal offset). Notional 10000, IM 1000 = full equity, MM 500.
  crossFill(m, BTC, 1, 2, 100, 50);
  crossFill(m, ETH, 1, 2, 100, 50);
  CHECK(m.maintenanceMargin(1) == usd(500));

  // BTC drops to 95: uPnl = -250 on BTC. equity = 1000 - 250 = 750.
  // MM = (95*50 + 100*50)*5% = (4750+5000)*0.05 = 487.5 -> still solvent.
  m.setMark(BTC, px(95));
  CHECK(liqs.empty());
  CHECK(m.positionQty(1, BTC) == qty(50).raw());

  // BTC crashes to 85: uPnl = -750. equity = 250. MM = (85*50+100*50)*5% = 462.5
  // -> 250 < 462.5 -> whole portfolio liquidated (both legs).
  m.setMark(BTC, px(85));
  CHECK(m.positionQty(1, BTC) == 0 && m.positionQty(1, ETH) == 0);
  CHECK(liqs.size() == 2);  // both symbols closed

  // Conservation across accounts + venue pool.
  CHECK(led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD) == init);
}

void test_bankruptcy_insurance()
{
  std::printf("test_cross_margin_bankruptcy\n");
  Ledger led;
  led.deposit(1, USD, usd(1000));
  led.deposit(2, USD, usd(100000));
  const Amount init = led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD);

  bool sawBankrupt = false;
  CrossMarginManager m(led, USD, VENUE, [&](const Liquidation& l)
                       { sawBankrupt = sawBankrupt || l.bankrupt; });
  m.configureSymbol(BTC, 1000, 500);
  m.setMark(BTC, px(100));

  crossFill(m, BTC, 1, 2, 100, 100);  // long 100 @ 100, notional 10000, IM 1000 = equity

  // Mark collapses to 80: uPnl = -2000 -> equity -1000 (negative). Liquidate;
  // wallet goes negative and insurance (venue) tops it up to zero.
  m.setMark(BTC, px(80));
  CHECK(m.positionQty(1, BTC) == 0);
  CHECK(led.available(1, USD) == 0);  // wiped out, insurance covered the deficit
  // Value conserved: the venue pool absorbed the loss (insurance drawdown).
  CHECK(led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD) == init);
}

// Auto-deleveraging: when a bankruptcy would drain the insurance fund, the
// deficit is instead clawed back from the most profitable opposite-side trader
// (their winning position is closed and their gain haircut). Insurance is spared.
void test_adl()
{
  std::printf("test_cross_margin_adl\n");
  Ledger led;
  led.deposit(1, USD, usd(1000));
  led.deposit(2, USD, usd(100000));
  const Amount init = led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD);

  std::vector<Liquidation> liqs;
  CrossMarginManager m(led, USD, VENUE, [&](const Liquidation& l)
                       { liqs.push_back(l); },
                       /*autoDeleverage*/ true);
  m.configureSymbol(BTC, 1000, 500);
  m.setMark(BTC, px(100));

  // Account 1 long 100 @ 100 (IM 1000 = full wallet); account 2 short 100 @ 100.
  crossFill(m, BTC, /*buyer*/ 1, /*seller*/ 2, 100, 100);
  const Amount venueBefore = led.total(VENUE, USD);  // insurance fund level

  // Mark crashes to 80: acct1 equity = 1000 - 2000 = -1000 (bankrupt, deficit 1000).
  m.setMark(BTC, px(80));
  CHECK(m.positionQty(1, BTC) == 0);
  CHECK(m.positionQty(2, BTC) == 0);  // winner was auto-deleveraged (closed)

  // Insurance fund untouched: the 1000 deficit was clawed from acct2's profit.
  CHECK(led.total(VENUE, USD) == venueBefore);
  // acct2's gain is capped at +1000 (to bankruptcy price) instead of +2000 (mark).
  CHECK(led.available(2, USD) == usd(100000) + usd(1000));

  bool sawAdl = false, sawBankrupt = false;
  for (const auto& l : liqs)
  {
    if (l.adl)
    {
      sawAdl = true;
    }
    if (l.bankrupt)
    {
      sawBankrupt = true;
    }
  }
  CHECK(sawAdl);
  CHECK(sawBankrupt);
  CHECK(led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD) == init);
}

// ADL victim selection must be a pure function of logical state: with two
// equal-uPnl winners and a deficit only one is needed to cover, the SAME winner
// (deterministic total order -> lowest account id) is deleveraged regardless of
// the order fills arrived in. Otherwise a replica rebuilt with a different
// insertion history would pick the other victim -> divergent balances + event
// hash, forking HA state. `winnersInReverse` flips the fill/insertion order.
uint64_t adl_victim(bool winnersInReverse)
{
  Ledger led;
  led.deposit(1, USD, usd(1000));    // bankrupt-to-be: long 100 @ 100, IM = wallet
  led.deposit(2, USD, usd(100000));  // deep winner A
  led.deposit(3, USD, usd(100000));  // deep winner B (identical position -> equal uPnl)
  std::vector<Liquidation> liqs;
  CrossMarginManager m(led, USD, VENUE, [&](const Liquidation& l)
                       { liqs.push_back(l); },
                       /*autoDeleverage*/ true);
  m.configureSymbol(BTC, 1000, 500);
  m.setMark(BTC, px(100));
  // Acct1 long 100 is matched by two 50-lot shorts (acct2, acct3). Fill order
  // (hence pos_ insertion order) is flipped by the flag.
  if (!winnersInReverse)
  {
    crossFill(m, BTC, /*buyer*/ 1, /*seller*/ 2, 100, 50);
    crossFill(m, BTC, /*buyer*/ 1, /*seller*/ 3, 100, 50);
  }
  else
  {
    crossFill(m, BTC, /*buyer*/ 1, /*seller*/ 3, 100, 50);
    crossFill(m, BTC, /*buyer*/ 1, /*seller*/ 2, 100, 50);
  }
  // Mark -> 80: acct1 equity = 1000 - 2000 = -1000 (deficit 1000). Each short
  // winner has uPnl = (100-80)*50 = +1000, so exactly ONE covers the deficit.
  m.setMark(BTC, px(80));
  uint64_t victim = 0;
  for (const auto& l : liqs)
  {
    if (l.adl)
    {
      victim = l.account;
    }
  }
  return victim;
}

void test_adl_deterministic_victim()
{
  std::printf("test_cross_margin_adl_deterministic_victim\n");
  const uint64_t v1 = adl_victim(/*winnersInReverse*/ false);
  const uint64_t v2 = adl_victim(/*winnersInReverse*/ true);
  CHECK(v1 == 2);   // lowest account id, per the total order
  CHECK(v2 == 2);   // same victim under the reversed insertion order
  CHECK(v1 == v2);  // insertion-order independent -> HA-safe
}

// ADL claw-back must never mint money. The confiscation debit is all-or-nothing
// and an ADL winner can have negative cash `available` (funding drives it there),
// so crediting the insurance fund the full haircut while the debit no-ops would
// create phantom money -- conservation must hold, with any uncovered remainder
// borne by insurance (the normal waterfall), not conjured.
void test_adl_negative_available_conserves()
{
  std::printf("test_cross_margin_adl_negative_available_conserves\n");
  Ledger led;
  led.deposit(1, USD, usd(100));     // B: bankrupt-to-be long
  led.deposit(2, USD, usd(120));     // W: ADL winner, about to be driven cash-negative
  led.deposit(3, USD, usd(100000));  // Z: ETH funding sink (isolates W's drain from B)
  auto total = [&]
  { return led.total(1, USD) + led.total(2, USD) + led.total(3, USD) + led.total(VENUE, USD); };
  const Amount init = total();

  std::vector<Liquidation> liqs;
  CrossMarginManager m(led, USD, VENUE, [&](const Liquidation& l)
                       { liqs.push_back(l); },
                       /*autoDeleverage*/ true);
  m.configureSymbol(BTC, 1000, 500);
  m.configureSymbol(ETH, 1000, 500);
  m.setMark(BTC, px(100));
  m.setMark(ETH, px(100));
  crossFill(m, BTC, /*buyer*/ 1, /*seller*/ 2, 100, 10);  // B long 10, W short 10
  crossFill(m, ETH, /*buyer*/ 2, /*seller*/ 3, 100, 2);   // W long 2 ETH, Z short 2 ETH

  // Drive W's cash negative via ETH funding, with liquidations paused so the
  // transient sub-maintenance state doesn't liquidate W before it becomes the
  // ADL winner. (Absurd rate -- we only need to reach the negative-cash state.)
  m.setLiquidationsPaused(true);
  m.applyFunding(ETH, /*rate*/ 1.15, px(100));  // W pays 230 -> available 120 - 230 = -110
  CHECK(led.available(2, USD) < 0);
  m.setLiquidationsPaused(false);

  // Crash BTC: B (long 10) goes bankrupt (loss 200 > deposit 100 -> deficit 100),
  // ADL claws from W (short 10, +200 uPnl). After the +200 credit W has 90 cash <
  // the 100 haircut, so the all-or-nothing debit is short.
  m.setMark(BTC, px(80));

  bool sawAdl = false;
  for (auto& l : liqs)
  {
    if (l.adl)
    {
      sawAdl = true;
    }
  }
  CHECK(sawAdl);
  CHECK(total() == init);  // no phantom money minted by the claw-back
}

// A trader must not withdraw collateral that backs open positions.
void test_withdrawal_gate()
{
  std::printf("test_cross_margin_withdrawal_gate\n");
  Ledger led;
  led.deposit(1, USD, usd(1000));
  CrossMarginManager m(led, USD, VENUE);
  m.configureSymbol(BTC, /*im*/ 1000, /*mm*/ 500);
  m.setMark(BTC, px(100));

  // Flat: the whole wallet is withdrawable.
  CHECK(m.withdrawable(1) == usd(1000));
  CHECK(m.canWithdraw(1, usd(1000)));
  CHECK(!m.canWithdraw(1, usd(1001)));

  // Open long 50 @ 100: notional 5000, IM 500. Free = equity 1000 - IM 500 = 500.
  crossFill(m, BTC, /*buyer*/ 1, /*seller*/ 2, 100, 50);
  CHECK(m.withdrawable(1) == usd(500));
  CHECK(m.canWithdraw(1, usd(500)));
  CHECK(!m.canWithdraw(1, usd(600)));  // would leave IM uncovered

  // A gain lifts equity and thus the withdrawable amount.
  m.setMark(BTC, px(110));  // uPnl = 50*(110-100) = +500; IM = 110*50*10% = 550
  CHECK(m.equity(1) == usd(1500));
  CHECK(m.withdrawable(1) == usd(1500) - usd(550));

  // A negative amount is never valid.
  CHECK(!m.canWithdraw(1, -usd(1)));
}

// Liquidations paused (feed outage): a bad mark must not liquidate the book.
void test_liquidations_paused()
{
  std::printf("test_cross_margin_liquidations_paused\n");
  Ledger led;
  led.deposit(1, USD, usd(1000));
  led.deposit(2, USD, usd(100000));
  std::vector<Liquidation> liqs;
  CrossMarginManager m(led, USD, VENUE, [&](const Liquidation& l)
                       { liqs.push_back(l); });
  m.configureSymbol(BTC, 1000, 500);
  m.setMark(BTC, px(100));
  crossFill(m, BTC, 1, 2, 100, 100);  // acct1 long 100, IM 1000 = wallet

  // Feed goes bad; operator pauses liquidations. A crash mark does NOT liquidate.
  m.setLiquidationsPaused(true);
  m.setMark(BTC, px(80));  // equity would be -1000
  CHECK(liqs.empty());
  CHECK(m.positionQty(1, BTC) == qty(100).raw());  // still open
  CHECK(m.equity(1) < 0);                          // PnL still reflects the mark

  // Feed recovers, liquidations resume; the next mark update sweeps.
  m.setLiquidationsPaused(false);
  m.setMark(BTC, px(80));
  CHECK(!liqs.empty());
  CHECK(m.positionQty(1, BTC) == 0);
}

// Multi-asset collateral: BTC posted as margin gives quote-denominated buying
// power (after haircut), so a trader with no quote can still open a position.
void test_multi_collateral()
{
  std::printf("test_cross_margin_multi_collateral\n");
  constexpr AssetId BTC_ASSET = 5;  // collateral asset (distinct from the perp symbol)
  Ledger led;
  led.deposit(1, BTC_ASSET, amountOf(Volume::fromDouble(1.0)));  // 1 BTC, zero USD

  CollateralSchedule sched;
  sched.configure(USD, px(1.0).raw(), /*haircut*/ 0);
  sched.configure(BTC_ASSET, px(30000).raw(), /*haircut*/ 2000);  // 20% -> 24000 credit

  CrossMarginManager m(led, USD, VENUE);
  m.setCollateralSchedule(&sched);
  m.configureSymbol(BTC, /*im*/ 1000, /*mm*/ 500);
  m.setMark(BTC, px(100));

  // Equity comes entirely from the haircut-adjusted BTC collateral.
  CHECK(m.equity(1) == usd(24000));
  CHECK(m.withdrawable(1) == usd(24000));  // flat -> all withdrawable

  // Buying power reflects the BTC-backed equity: 200000 notional (IM 20000) fits;
  // 250000 (IM 25000) exceeds the 24000 equity.
  CHECK(m.canOpen(1, BTC, Side::BUY, qty(2000).raw(), px(100).raw()));
  CHECK(!m.canOpen(1, BTC, Side::BUY, qty(2500).raw(), px(100).raw()));
}

// On liquidation, a non-quote collateral basket is sold to the venue to cover
// the quote loss before the insurance fund is tapped; value stays conserved
// per asset (the venue ends up holding the coin).
void test_collateral_liquidation()
{
  std::printf("test_cross_margin_collateral_liquidation\n");
  constexpr AssetId BTC_ASSET = 5;
  const Amount half = amountOf(Volume::fromDouble(0.05));  // 0.05 BTC = 1500 USD @ 30000
  Ledger led;
  led.deposit(1, BTC_ASSET, half);
  led.deposit(2, USD, usd(100000));
  const Amount initBtc = led.total(1, BTC_ASSET) + led.total(2, BTC_ASSET) + led.total(VENUE, BTC_ASSET);
  const Amount initUsd = led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD);

  CollateralSchedule sched;
  sched.configure(USD, px(1.0).raw(), 0);
  sched.configure(BTC_ASSET, px(30000).raw(), 0);  // 0 haircut -> clean math

  std::vector<Liquidation> liqs;
  CrossMarginManager m(led, USD, VENUE, [&](const Liquidation& l)
                       { liqs.push_back(l); });
  m.setCollateralSchedule(&sched);
  m.configureSymbol(BTC, /*im*/ 1000, /*mm*/ 500);
  m.setMark(BTC, px(100));
  CHECK(m.equity(1) == usd(1500));

  crossFill(m, BTC, /*buyer*/ 1, /*seller*/ 2, 100, 100);  // acct1 long 100 (IM 1000 < 1500)

  // Crash to 80: acct1 uPnl -2000, equity -500 -> liquidate. The 2000 loss is
  // covered first by selling the 1500 of BTC collateral, then 500 by insurance.
  m.setMark(BTC, px(80));
  CHECK(m.positionQty(1, BTC) == 0);
  CHECK(led.total(1, BTC_ASSET) == 0);         // collateral fully sold
  CHECK(led.total(VENUE, BTC_ASSET) == half);  // venue now holds the coin
  CHECK(led.available(1, USD) == 0);           // wallet topped to zero

  // Value conserved per asset.
  CHECK(led.total(1, BTC_ASSET) + led.total(2, BTC_ASSET) + led.total(VENUE, BTC_ASSET) == initBtc);
  CHECK(led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD) == initUsd);

  bool bankrupt = false;
  for (const auto& l : liqs)
  {
    if (l.bankrupt)
    {
      bankrupt = true;
    }
  }
  CHECK(bankrupt);  // collateral covered 1500, insurance the residual 500
}

// Funding across the portfolio book: longs pay shorts, pool nets zero, conserved.
void test_funding()
{
  std::printf("test_cross_margin_funding\n");
  Ledger led;
  led.deposit(1, USD, usd(100000));
  led.deposit(2, USD, usd(100000));
  const Amount init = led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD);
  CrossMarginManager m(led, USD, VENUE);
  m.configureSymbol(BTC, 1000, 500);
  m.setMark(BTC, px(100));
  crossFill(m, BTC, /*buyer*/ 1, /*seller*/ 2, 100, 100);  // acct1 long, acct2 short, notional 10000

  const Amount a1 = led.available(1, USD);
  const Amount a2 = led.available(2, USD);
  m.applyFunding(BTC, /*rate*/ 0.01, px(100));    // 1% of 10000 = 100
  CHECK(led.available(1, USD) == a1 - usd(100));  // long paid 100
  CHECK(led.available(2, USD) == a2 + usd(100));  // short received 100
  CHECK(led.total(VENUE, USD) == 0);              // pool net zero (balanced book)
  CHECK(led.total(1, USD) + led.total(2, USD) + led.total(VENUE, USD) == init);
}

// Funding that drains a thin wallet below maintenance triggers a liquidation in
// the same applyFunding call (the sweep the fix added).
void test_funding_liquidation()
{
  std::printf("test_cross_margin_funding_liquidation\n");
  Ledger led;
  led.deposit(1, USD, usd(1000));  // long: wallet == IM, thin
  led.deposit(2, USD, usd(100000));
  std::vector<Liquidation> liqs;
  CrossMarginManager m(led, USD, VENUE, [&](const Liquidation& l)
                       { liqs.push_back(l); });
  m.configureSymbol(BTC, /*im*/ 1000, /*mm*/ 500);
  m.setMark(BTC, px(100));
  crossFill(m, BTC, 1, 2, 100, 100);  // acct1 long 100 @ 100, notional 10000, MM 500
  CHECK(m.equity(1) == usd(1000));

  // A steep 6% funding: long pays 600 -> equity 400 < MM 500 -> liquidated.
  m.applyFunding(BTC, 0.06, px(100));
  CHECK(!liqs.empty());
  CHECK(m.positionQty(1, BTC) == 0);
}

}  // namespace

TEST(CrossMargin, EngineSuite)
{
  test_portfolio_im_gate();
  test_multi_collateral();
  test_collateral_liquidation();
  test_funding();
  test_funding_liquidation();
  test_cross_offset();
  test_cross_liquidation();
  test_bankruptcy_insurance();
  test_adl();
  test_adl_deterministic_victim();
  test_adl_negative_available_conserves();
  test_withdrawal_gate();
  test_liquidations_paused();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
