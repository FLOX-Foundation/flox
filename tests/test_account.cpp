/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/account.h"
#include "flox/backtest/fee_schedule.h"
#include "flox/backtest/liquidation_engine.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;
constexpr SymbolId ETH = 2;
}  // namespace

// === Account basics ===

TEST(Account, DefaultsToCrossMode)
{
  Account a(42, 10'000.0);
  EXPECT_EQ(a.accountId(), 42u);
  EXPECT_DOUBLE_EQ(a.equity(), 10'000.0);
  EXPECT_EQ(a.marginMode(), MarginMode::Cross);
}

TEST(Account, MarginModeByNameAcceptsKnownStrings)
{
  Account a;
  a.setMarginModeByName("isolated");
  EXPECT_EQ(a.marginMode(), MarginMode::Isolated);
  a.setMarginModeByName("cross");
  EXPECT_EQ(a.marginMode(), MarginMode::Cross);
  // Unknown is a no-op.
  a.setMarginModeByName("garbage");
  EXPECT_EQ(a.marginMode(), MarginMode::Cross);
}

TEST(Account, PositionBookOpenAndClose)
{
  Account a(1, 1000.0);
  a.openPosition(BTC, 5.0, 50'000.0);
  a.openPosition(ETH, -10.0, 3'000.0);
  EXPECT_EQ(a.positionCount(), 2u);
  a.closePosition(BTC);
  EXPECT_EQ(a.positionCount(), 1u);
  EXPECT_EQ(a.positions().front().symbol, ETH);
}

TEST(Account, MarksDefaultToEntryWhenUnset)
{
  Account a(1, 1000.0);
  a.openPosition(BTC, 5.0, 50'000.0);
  // No mark set → totalNotional uses entry price.
  EXPECT_DOUBLE_EQ(a.totalNotional(), 5.0 * 50'000.0);
  EXPECT_DOUBLE_EQ(a.totalUnrealisedPnl(), 0.0);
}

TEST(Account, MarksProduceCrossUpnl)
{
  Account a(1, 1000.0);
  a.openPosition(BTC, 5.0, 50'000.0);
  a.openPosition(ETH, -10.0, 3'000.0);
  a.setMark(BTC, 49'000.0);  // long underwater: -5000
  a.setMark(ETH, 2'800.0);   // short profitable: +2000
  EXPECT_DOUBLE_EQ(a.totalUnrealisedPnl(), -5000.0 + 2000.0);
}

// === Cross-margin liquidation ===

TEST(Account, CrossMarginSurvivesWhenOneLegProfitable)
{
  // BTC long underwater + ETH short profitable → account safe.
  Account a(1, 10'000.0);
  a.openPosition(BTC, 1.0, 50'000.0);
  a.openPosition(ETH, -10.0, 3'000.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);  // 0.5% MM
  e.attachAccount(&a);

  // BTC at 47k: long uPnL = -3000. ETH still at entry → 0 uPnL.
  // Account: equity 10k + (-3000) = 7000; notional = 47k + 30k = 77k;
  // mmReq = 385. Safe.
  a.setMark(ETH, 3'000.0);
  const auto out = e.onMark(BTC, 47'000.0);
  EXPECT_EQ(out.liquidationsCount, 0u);
  EXPECT_EQ(a.positionCount(), 2u);

  // Push BTC lower but ETH profitable: BTC at 40k (-10000) + ETH at
  // 2'200 (+8000) → net upnl -2000. Equity 10k - 2k = 8k. Notional
  // 40k + 22k = 62k. Cushion 8k vs req 310. Still safe.
  a.setMark(ETH, 2'200.0);
  const auto out2 = e.onMark(BTC, 40'000.0);
  EXPECT_EQ(out2.liquidationsCount, 0u);
  EXPECT_EQ(a.positionCount(), 2u);
}

TEST(Account, CrossMarginLiquidatesWorstLegWhenBothUnderwater)
{
  // BTC long underwater AND ETH long underwater → both bleed.
  // Engine should close the worst-PnL leg first.
  Account a(1, 1'000.0);
  a.openPosition(BTC, 1.0, 50'000.0);  // notional 50k
  a.openPosition(ETH, 10.0, 3'000.0);  // notional 30k

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.attachAccount(&a);

  // BTC mark drops 10% (-5000), ETH drops 30% (-9000).
  // Account: equity 1000 + (-14000) = -13000. notional 45k + 21k = 66k.
  // Way underwater. Engine should liquidate ETH first (most-negative).
  a.setMark(ETH, 2'100.0);
  const auto out = e.onMark(BTC, 45'000.0);
  // At least one liquidation; ETH closes first.
  EXPECT_GE(out.liquidationsCount, 1u);
  // ETH should be gone (worst-PnL closed first).
  bool ethStillOpen = false;
  for (const auto& p : a.positions())
  {
    if (p.symbol == ETH)
    {
      ethStillOpen = true;
      break;
    }
  }
  EXPECT_FALSE(ethStillOpen) << "ETH (worst leg) should liquidate first";
}

TEST(Account, IsolatedModeDoesNotShareEquity)
{
  // Isolated mode: account-level cross check is skipped. Per-position
  // liquidation still works via the engine's standalone position book
  // — this asserts that attaching an Isolated account does NOT trigger
  // cross-margin liquidation of an otherwise-survivable account.
  Account a(1, 1'000.0);
  a.setMarginMode(MarginMode::Isolated);
  a.openPosition(BTC, 1.0, 50'000.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.attachAccount(&a);

  // BTC drops 1% (-500). In Cross mode equity 1000 - 500 = 500
  // would still survive; here Isolated mode means the engine
  // doesn't run the cross check at all. The position lacks a
  // per-position equity slice → it survives because there's no
  // cross-trigger.
  const auto out = e.onMark(BTC, 49'500.0);
  EXPECT_EQ(out.liquidationsCount, 0u);
  EXPECT_EQ(a.positionCount(), 1u);
}

// === Cross-account 30d notional binding for FeeSchedule ===

TEST(Account, FeeScheduleBoundReadsAccountRollingNotional)
{
  Account a(1, 10'000.0);
  FeeSchedule s = FeeSchedule::binance_um_futures();
  s.bindAccount(&a);

  // recordFill via FeeSchedule pushes into the account's counter.
  s.recordFill(0, 100'000.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 100'000.0);
  EXPECT_DOUBLE_EQ(s.rollingNotional30d(), 100'000.0);

  // Direct account-level fills (e.g., from other symbols) also
  // count towards the same rolling sum.
  a.recordFill(0, 200'000.0);
  EXPECT_DOUBLE_EQ(s.rollingNotional30d(), 300'000.0);

  s.clearAccountBinding();
  EXPECT_EQ(s.boundAccount(), nullptr);
  // After unbind, schedule's internal counter (which was 100k from
  // the bound-mode recordFill — wait, no: bound mode skipped the
  // internal counter). So internal is 0.
  EXPECT_DOUBLE_EQ(s.rollingNotional30d(), 0.0);
}

TEST(Account, FeeScheduleCrossSymbolNotionalTriggersTier)
{
  // 30d notional accumulated across BTC + ETH trades crosses the
  // VIP 1 tier threshold (250k for binance_um_futures). Without
  // an account, per-symbol FeeSchedules would each fall below;
  // with the account binding, aggregate crosses.
  Account a(1, 100'000.0);

  FeeSchedule btcSched = FeeSchedule::binance_um_futures();
  FeeSchedule ethSched = FeeSchedule::binance_um_futures();
  btcSched.bindAccount(&a);
  ethSched.bindAccount(&a);

  btcSched.recordFill(0, 150'000.0);
  ethSched.recordFill(0, 150'000.0);

  // Aggregate is 300k → past 250k VIP 1 threshold.
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 300'000.0);
  EXPECT_GE(btcSched.currentTierIndex(), 1u);
  EXPECT_GE(ethSched.currentTierIndex(), 1u);
}

TEST(Account, FeeScheduleUnboundCounterUnaffectedByAccount)
{
  Account a(1, 100'000.0);
  FeeSchedule s;
  s.addTier(0, 2.0, 4.0);
  s.addTier(250'000, 1.6, 4.0);
  // No bindAccount; account-side fills must not leak into the schedule.
  a.recordFill(0, 500'000.0);
  s.recordFill(0, 100'000.0);
  EXPECT_DOUBLE_EQ(s.rollingNotional30d(), 100'000.0);
  EXPECT_EQ(s.currentTierIndex(), 0u);
}

// === T053: multi-symbol marks auto-sync + stale-mark guard ===

TEST(Account, MarkTsRecordedOnSetMark)
{
  Account a(1, 1000.0);
  a.setMark(BTC, 50'000.0, /*tsNs=*/12345);
  EXPECT_EQ(a.markTsFor(BTC), 12345);
  EXPECT_EQ(a.markTsFor(ETH), INT64_MIN);  // never marked

  // Re-marking overwrites both price and ts.
  a.setMark(BTC, 51'000.0, /*tsNs=*/67890);
  EXPECT_DOUBLE_EQ(a.markFor(BTC), 51'000.0);
  EXPECT_EQ(a.markTsFor(BTC), 67890);
}

TEST(Account, HasStaleMarksFlagsUnmarkedSymbols)
{
  Account a(1, 10'000.0);
  a.openPosition(BTC, 1.0, 50'000.0);
  // No marks set → BTC position counts as stale.
  EXPECT_TRUE(a.hasStaleMarks(/*nowNs=*/1'000'000'000, /*budgetNs=*/60'000'000'000));

  a.setMark(BTC, 50'000.0, /*tsNs=*/500'000'000);
  EXPECT_FALSE(
      a.hasStaleMarks(/*nowNs=*/1'000'000'000, /*budgetNs=*/60'000'000'000));
}

TEST(Account, HasStaleMarksFlagsBeyondBudget)
{
  Account a(1, 10'000.0);
  a.openPosition(BTC, 1.0, 50'000.0);
  a.setMark(BTC, 50'000.0, /*tsNs=*/0);
  // Budget is 1 second; now is 2 seconds in future → stale.
  EXPECT_TRUE(
      a.hasStaleMarks(/*nowNs=*/2'000'000'000, /*budgetNs=*/1'000'000'000));
  // Fresh enough.
  EXPECT_FALSE(
      a.hasStaleMarks(/*nowNs=*/500'000'000, /*budgetNs=*/1'000'000'000));
}

TEST(Account, MultiSymbolOnMarksUpdatesAllAccountsAtomically)
{
  // Cross-margin survival depends on knowing BOTH BTC and ETH
  // marks. With per-symbol onMark, the ETH leg would stay stale
  // until the caller manually called set_mark for ETH. onMarks
  // batches the update so the walk sees all current marks at once.
  Account a(1, 10'000.0);
  a.openPosition(BTC, 1.0, 50'000.0);   // long BTC
  a.openPosition(ETH, -10.0, 3'000.0);  // short ETH

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.attachAccount(&a);

  // BTC drops 6% (-3000 uPnL); ETH drops 25% (+7500 uPnL).
  // Net account uPnL: +4500. Equity 10k + 4500 = 14.5k. Safe.
  std::vector<std::pair<SymbolId, double>> marks = {{BTC, 47'000.0}, {ETH, 2'250.0}};
  const auto out = e.onMarks(marks);
  EXPECT_EQ(out.liquidationsCount, 0u);
  EXPECT_EQ(a.positionCount(), 2u);
  EXPECT_DOUBLE_EQ(a.markFor(BTC), 47'000.0);
  EXPECT_DOUBLE_EQ(a.markFor(ETH), 2'250.0);
}
