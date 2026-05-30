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

TEST(Account, IsolatedModePositionLiquidatesIndependently)
{
  // Isolated mode: each position carries its own posted-margin slice.
  // The engine walks them per-position; netting across symbols does
  // NOT happen.
  Account a(1, 0.0);  // account equity unused in isolated mode
  a.setMarginMode(MarginMode::Isolated);
  // BTC long with 1000 isolated equity at entry 50k.
  a.openPosition(BTC, 1.0, 50'000.0, /*isolated_equity=*/1'000.0);
  // ETH short with 500 isolated equity at entry 3k.
  a.openPosition(ETH, -10.0, 3'000.0, /*isolated_equity=*/500.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.attachAccount(&a);

  // BTC drops 5% (-2500 uPnL) → 1000 + (-2500) < req → liquidate.
  // ETH untouched (separate symbol; mark not called).
  const auto out = e.onMark(BTC, 47'500.0);
  EXPECT_GE(out.liquidationsCount, 1u);
  // ETH still in book.
  EXPECT_EQ(a.positionCount(), 1u);
  EXPECT_EQ(a.positions().front().symbol, ETH);
}

TEST(Account, IsolatedModeNettingDoesNotShelter)
{
  // Same scenario as CrossMarginSurvivesWhenOneLegProfitable but
  // in isolated mode: the profitable ETH short does NOT shelter the
  // underwater BTC long.
  Account a(1, 0.0);
  a.setMarginMode(MarginMode::Isolated);
  a.openPosition(BTC, 1.0, 50'000.0, /*isolated_equity=*/100.0);
  a.openPosition(ETH, -10.0, 3'000.0, /*isolated_equity=*/10'000.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.attachAccount(&a);

  // BTC drops 1% (-500 uPnL); 100 + (-500) underwater. ETH stays
  // healthy in its own bucket. Engine should liquidate only BTC.
  a.setMark(ETH, 3'000.0);
  const auto out = e.onMark(BTC, 49'500.0);
  EXPECT_GE(out.liquidationsCount, 1u);
  EXPECT_EQ(a.positionCount(), 1u);
  EXPECT_EQ(a.positions().front().symbol, ETH);
}

// === T060: 30-day rolling-window eviction ===

namespace
{
constexpr int64_t k30dNs = 30LL * 24LL * 3600LL * 1'000'000'000LL;
}  // namespace

TEST(Account, RollingNotionalSingleFillNoEviction)
{
  Account a(1, 0.0);
  a.recordFill(/*tsNs=*/0, /*notional=*/100'000.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 100'000.0);

  // Second fill within 30d → both counted.
  a.recordFill(/*tsNs=*/k30dNs / 2, /*notional=*/50'000.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 150'000.0);
}

TEST(Account, RollingNotionalEvictsExactlyAtBoundary)
{
  Account a(1, 0.0);
  a.recordFill(/*tsNs=*/0, /*notional=*/100'000.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 100'000.0);

  // A new fill at exactly 30d after the first triggers eviction of
  // the first (evictExpired uses `ts <= cutoff` where
  // cutoff = nowNs - 30d, so a fill at nowNs = 30d makes cutoff = 0
  // and the t=0 fill drops).
  a.recordFill(/*tsNs=*/k30dNs, /*notional=*/40'000.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 40'000.0);
}

TEST(Account, RollingNotionalEvictsMultipleAcrossBoundary)
{
  Account a(1, 0.0);
  // Three fills sprinkled across the early window.
  a.recordFill(0, 10'000.0);
  a.recordFill(k30dNs / 4, 20'000.0);  // 7.5d
  a.recordFill(k30dNs / 2, 30'000.0);  // 15d
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 60'000.0);

  // Jump 31 days forward → cutoff = 1d, only t=0 fill evicted.
  // t=7.5d and t=15d stay (both > 1d before now=31d).
  const int64_t day = 24LL * 3600LL * 1'000'000'000LL;
  a.recordFill(31LL * day, 5'000.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 20'000.0 + 30'000.0 + 5'000.0);

  // Jump further (100d total) — every prior fill is past cutoff
  // (100d - 30d = 70d; all fills below 70d are dropped).
  a.recordFill(100LL * day, 1'000.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 1'000.0);
}

TEST(Account, RollingNotionalLargeFarFutureClearsWindow)
{
  Account a(1, 0.0);
  for (int i = 0; i < 10; ++i)
  {
    a.recordFill(static_cast<int64_t>(i) * 1'000'000'000LL, 10'000.0);
  }
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 100'000.0);

  // Fill 100 days later evicts everything prior.
  a.recordFill(100LL * 24LL * 3600LL * 1'000'000'000LL, 7.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 7.0);
}

TEST(Account, RollingNotionalResetClearsCounter)
{
  Account a(1, 0.0);
  a.recordFill(0, 500'000.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 500'000.0);
  a.resetRolling();
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 0.0);
}

TEST(Account, FeeScheduleRollingNotionalEvicts)
{
  // FeeSchedule has its own copy of the rolling-window logic; verify
  // eviction works there too. Same eviction policy as Account.
  FeeSchedule s = FeeSchedule::binance_um_futures();
  s.recordFill(0, 200'000.0);
  EXPECT_DOUBLE_EQ(s.rollingNotional30d(), 200'000.0);

  // Fill 31 days later evicts the earlier one.
  s.recordFill(31LL * 24LL * 3600LL * 1'000'000'000LL, 50'000.0);
  EXPECT_DOUBLE_EQ(s.rollingNotional30d(), 50'000.0);

  // After eviction we no longer qualify for VIP 1 (>=250k).
  EXPECT_EQ(s.currentTierIndex(), 0u);
}

TEST(Account, BoundAccountEvictionVisibleViaFeeSchedule)
{
  // When a FeeSchedule is bound to an Account, the schedule reads
  // the account's rolling notional. Eviction on the account-side
  // should be reflected in the schedule's tier resolution.
  Account a(1, 0.0);
  FeeSchedule s = FeeSchedule::binance_um_futures();
  s.bindAccount(&a);

  s.recordFill(0, 300'000.0);
  EXPECT_GE(s.currentTierIndex(), 1u);  // crossed VIP 1

  // 31 days later: eviction kicks the earlier fill out. Record a
  // small fill at the far end to trigger eviction (which only
  // happens on recordFill).
  s.recordFill(31LL * 24LL * 3600LL * 1'000'000'000LL, 1'000.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 1'000.0);
  EXPECT_EQ(s.currentTierIndex(), 0u);
}

// === T055: cross-account ADL routing ===

TEST(Account, CrossAccountDeficitDepletesInsuranceFund)
{
  // One underwater cross-margin account, no opposite-side ADL pool
  // available. Insurance fund covers some of the deficit; the rest
  // (with ADL disabled here for isolation) goes unmet, which is
  // visible via insurance_fund_balance.
  Account a(1, 0.0);
  a.openPosition(BTC, 1.0, 50'000.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(5'000.0);
  e.setAdlEnabled(false);
  e.attachAccount(&a);

  // BTC drops 20% (-10000). Account: equity 0 + (-10000) far below
  // mmReq. Liquidates the BTC leg, leaves a 10k deficit. Insurance
  // fund covers 5k; ADL disabled so the rest is unmet.
  const auto out = e.onMark(BTC, 40'000.0);
  EXPECT_GE(out.liquidationsCount, 1u);
  EXPECT_DOUBLE_EQ(e.insuranceFundBalance(), 0.0);
  EXPECT_GE(out.insurancePaymentsCount, 1u);
}

TEST(Account, CrossAccountDeficitTriggersAdlAcrossAccounts)
{
  // Two cross accounts: one is liquidated and leaves a deficit, the
  // other holds a profitable opposite-side position on the same
  // symbol. After insurance is depleted, ADL closes the profitable
  // position on the other account and credits its equity with the
  // realised PnL.
  Account aLong(1, 0.0);
  aLong.openPosition(BTC, 1.0, 50'000.0);  // gets liquidated

  Account aShort(2, 5'000.0);
  aShort.openPosition(BTC, -1.0, 50'000.0);  // profits on drop

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(0.0);  // no insurance — straight to ADL
  e.setAdlEnabled(true);
  e.attachAccount(&aLong);
  e.attachAccount(&aShort);

  // BTC drops 20% (-10000 for aLong, +10000 for aShort). aLong
  // liquidates with 10k deficit. ADL closes aShort's profitable
  // BTC short to absorb the deficit; aShort's equity is credited
  // with the realised +10k.
  const double aShortEquityBefore = aShort.equity();
  const auto out = e.onMark(BTC, 40'000.0);
  EXPECT_GE(out.liquidationsCount, 1u);
  EXPECT_GE(out.adlCloseoutsCount, 1u);
  // aShort's BTC position should be gone (ADL'd).
  EXPECT_EQ(aShort.positionCount(), 0u);
  // aShort's equity rose by the realised +10k from the ADL close.
  EXPECT_GT(aShort.equity(), aShortEquityBefore);
}

TEST(Account, IsolatedAndCrossAccountsCoexist)
{
  // One cross account, one isolated account on the same engine.
  // Engine should walk each correctly under its own rules.
  Account aCross(1, 5'000.0);
  aCross.openPosition(BTC, 1.0, 50'000.0);
  aCross.openPosition(ETH, -10.0, 3'000.0);

  Account aIsolated(2, 0.0);
  aIsolated.setMarginMode(MarginMode::Isolated);
  aIsolated.openPosition(BTC, 1.0, 50'000.0, /*isolated_equity=*/200.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.attachAccount(&aCross);
  e.attachAccount(&aIsolated);

  // BTC down 1% (-500): cross sees net (-500 + profitable ETH);
  // 5000 equity easily covers. Isolated sees -500 vs 200 equity →
  // liquidate.
  aCross.setMark(ETH, 3'000.0);
  const auto out = e.onMark(BTC, 49'500.0);
  EXPECT_GE(out.liquidationsCount, 1u);
  EXPECT_EQ(aCross.positionCount(), 2u);     // cross survives
  EXPECT_EQ(aIsolated.positionCount(), 0u);  // isolated liquidated
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

// === T059: per-symbol record_fill breakdown ===

TEST(Account, RecordFillDefaultsToZeroSymbol)
{
  Account a(1, 0.0);
  a.recordFill(0, 100'000.0);
  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 100'000.0);
  const auto by = a.rollingNotionalBySymbol30d();
  ASSERT_EQ(by.size(), 1u);
  EXPECT_EQ(by[0].first, 0u);
  EXPECT_DOUBLE_EQ(by[0].second, 100'000.0);
}

TEST(Account, RecordFillTagsBySymbol)
{
  Account a(1, 0.0);
  a.recordFill(0, 100'000.0, BTC);
  a.recordFill(0, 50'000.0, ETH);
  a.recordFill(0, 25'000.0, BTC);  // second BTC fill

  EXPECT_DOUBLE_EQ(a.rollingNotional30d(), 175'000.0);
  const auto by = a.rollingNotionalBySymbol30d();
  ASSERT_EQ(by.size(), 2u);
  EXPECT_EQ(by[0].first, BTC);
  EXPECT_DOUBLE_EQ(by[0].second, 125'000.0);
  EXPECT_EQ(by[1].first, ETH);
  EXPECT_DOUBLE_EQ(by[1].second, 50'000.0);
}

TEST(Account, PerSymbolBreakdownRespectsEviction)
{
  Account a(1, 0.0);
  const int64_t day = 24LL * 3600LL * 1'000'000'000LL;
  a.recordFill(0, 100'000.0, BTC);
  a.recordFill(15LL * day, 50'000.0, ETH);

  auto by = a.rollingNotionalBySymbol30d();
  ASSERT_EQ(by.size(), 2u);

  // 35 days later: BTC fill (t=0) past cutoff, evicted.
  a.recordFill(35LL * day, 10'000.0, ETH);
  by = a.rollingNotionalBySymbol30d();
  ASSERT_EQ(by.size(), 1u);
  EXPECT_EQ(by[0].first, ETH);
  EXPECT_DOUBLE_EQ(by[0].second, 50'000.0 + 10'000.0);
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

// === Contract multiplier + option premium semantics (W16-T020) ===

namespace
{
constexpr SymbolId ES = 3;   // an index future, multiplier 50
constexpr SymbolId OPT = 4;  // an option, multiplier 100
}  // namespace

TEST(Account, MultiplierScalesNotionalAndUpnl)
{
  Account a(1, 100'000.0);
  // ES future: 2 contracts at 4000, multiplier 50.
  a.openPosition(ES, 2.0, 4'000.0, /*isolatedEquity=*/0.0, /*contractMultiplier=*/50.0);
  a.setMark(ES, 4'010.0);

  EXPECT_DOUBLE_EQ(a.totalNotional(), 2.0 * 4'010.0 * 50.0);    // 401,000
  EXPECT_DOUBLE_EQ(a.totalUnrealisedPnl(), 2.0 * 10.0 * 50.0);  // 1,000
  // No long options here, so margin aggregates equal the totals.
  EXPECT_DOUBLE_EQ(a.marginNotional(), a.totalNotional());
  EXPECT_DOUBLE_EQ(a.marginUnrealisedPnl(), a.totalUnrealisedPnl());
}

TEST(Account, PerpUnchangedByMultiplierDefault)
{
  Account a(1, 1'000.0);
  a.openPosition(BTC, 1.0, 50'000.0);  // default multiplier 1.0, not an option
  a.setMark(BTC, 49'000.0);
  EXPECT_DOUBLE_EQ(a.totalNotional(), 49'000.0);
  EXPECT_DOUBLE_EQ(a.marginNotional(), 49'000.0);
  EXPECT_DOUBLE_EQ(a.totalUnrealisedPnl(), -1'000.0);
  EXPECT_DOUBLE_EQ(a.crossHeadroom(0.005), 1'000.0 - 1'000.0 - 49'000.0 * 0.005);
}

TEST(Account, LongOptionNotLiquidatedOnAdverseMark)
{
  // A long option's loss is the premium already paid; it posts no maintenance
  // margin and must never trigger a cross-margin liquidation.
  Account a(1, 1'000.0);
  a.openPosition(OPT, 1.0, 50.0, /*isolatedEquity=*/0.0, /*contractMultiplier=*/100.0,
                 /*isLongOption=*/true);
  a.setMark(OPT, 1.0);  // option collapses toward worthless

  // Carved out of the margin requirement: headroom is just the equity.
  EXPECT_DOUBLE_EQ(a.marginNotional(), 0.0);
  EXPECT_DOUBLE_EQ(a.marginUnrealisedPnl(), 0.0);
  EXPECT_DOUBLE_EQ(a.crossHeadroom(0.005), 1'000.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.attachAccount(&a);
  const auto out = e.onMark(OPT, 1.0);
  EXPECT_EQ(out.liquidationsCount, 0u);
  EXPECT_EQ(a.positionCount(), 1u);  // survives
}

TEST(Account, ShortOptionIsMargined)
{
  // A short option carries real margin risk and IS subject to liquidation.
  Account a(1, 1'000.0);
  a.openPosition(OPT, -1.0, 50.0, /*isolatedEquity=*/0.0, /*contractMultiplier=*/100.0,
                 /*isLongOption=*/false);
  a.setMark(OPT, 120.0);  // short loses: -1 * (120 - 50) * 100 = -7000

  EXPECT_DOUBLE_EQ(a.marginNotional(), 1.0 * 120.0 * 100.0);  // 12,000 — margined
  EXPECT_DOUBLE_EQ(a.marginUnrealisedPnl(), -7'000.0);
  EXPECT_LT(a.crossHeadroom(0.005), 0.0);  // underwater

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.attachAccount(&a);
  const auto out = e.onMark(OPT, 120.0);
  EXPECT_GE(out.liquidationsCount, 1u);  // liquidated
}

TEST(Account, LongOptionDoesNotShieldMarginedLeg)
{
  // A perp underwater past maintenance margin still liquidates even with a long
  // option in the book — the option is skipped as a closeout victim.
  Account a(1, 1'000.0);
  a.openPosition(BTC, 1.0, 50'000.0);  // margined perp
  a.openPosition(OPT, 1.0, 50.0, /*isolatedEquity=*/0.0, /*contractMultiplier=*/100.0,
                 /*isLongOption=*/true);
  a.setMark(BTC, 44'000.0);  // -6000 on the perp
  a.setMark(OPT, 1.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.attachAccount(&a);
  const auto out = e.onMark(BTC, 44'000.0);
  EXPECT_GE(out.liquidationsCount, 1u);
  // The long option is never the victim: it remains in the book.
  bool optStillOpen = false;
  for (const auto& p : a.positions())
  {
    if (p.symbol == OPT)
    {
      optStillOpen = true;
    }
  }
  EXPECT_TRUE(optStillOpen);
}
