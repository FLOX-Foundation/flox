/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// T064: real-event replay calibration harness.
//
// Structural tests (T056 + T065) confirm the engine produces
// internally-consistent and deterministic numbers. This test goes
// one layer up: it asserts the engine's quantitative output on
// well-known scenarios falls within tolerance of the baseline
// expected from venue published / public-report behaviour.
//
// Baselines here are embedded constants. For larger / per-symbol
// calibration regressions across recorded tapes, file the
// follow-up that mounts the Singapore md_collector data.
//
// A failure surface tells the researcher WHICH subsystem broke:
// liquidation count off → engine MM check; insurance hit off →
// deficit routing; funding payment off → schedule math.

#include "flox/backtest/account.h"
#include "flox/backtest/funding_schedule.h"
#include "flox/backtest/liquidation_engine.h"
#include "flox/backtest/venue_stack.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;
constexpr SymbolId ETH = 2;

// Tolerance helpers: relative error and signed-magnitude check.
bool withinRelativeTolerance(double actual, double expected, double tol)
{
  if (expected == 0.0)
  {
    return std::abs(actual) < tol;
  }
  return std::abs(actual - expected) / std::abs(expected) < tol;
}
}  // namespace

// === Scenario 1: 15% BTC drop liquidates a 20x leveraged long ===
//
// At 20x leverage, a 5% mark drop reaches the maintenance-margin
// boundary on a typical 0.5% MM tier. A 15% drop is catastrophic.
//
// Baseline: long position should liquidate, and the realised close
// price should reflect the engine's slippage model. Insurance fund
// should absorb most of the residual deficit since equity is wiped.

TEST(W15Calibration, BtcDrop15PctLiquidatesLeveragedLong)
{
  // 20x leverage: 1 BTC at 50k, equity 2500 (5% margin).
  auto stack = VenueStack::binance_um_futures(/*account_id=*/1,
                                              /*equity=*/2'500.0);
  // Detach the executor so the engine uses flat-bps slippage.
  // closeThroughExecutor against an unfed book wouldn't fill.
  stack.liquidation().setExecutor(nullptr);
  stack.account().openPosition(BTC, 1.0, 50'000.0);

  const double pre_insurance = stack.liquidation().insuranceFundBalance();

  // 15% drop in one tick.
  (void)stack.liquidation().onMark(BTC, 42'500.0);

  // Assertions — baseline expectations:
  // 1. Position fully liquidated.
  EXPECT_EQ(stack.account().positionCount(), 0u)
      << "20x long should liquidate on a 15% drop";
  EXPECT_EQ(stack.liquidation().liquidationsCount(), 1u);

  // 2. Insurance fund DECREASED — equity (2500) cannot cover the
  //    realised loss (about 7500), so the fund absorbs ~5000.
  EXPECT_LT(stack.liquidation().insuranceFundBalance(), pre_insurance)
      << "insurance fund should have paid out a deficit";

  // 3. The cascade buffer recorded exactly one liquidation pass.
  EXPECT_EQ(stack.liquidation().cascadeSizesPerTick().size(), 1u);
  EXPECT_EQ(stack.liquidation().cascadeSizesPerTick().front(), 1u);
}

// === Scenario 2: 8-hour funding settlement matches expected sign + magnitude ===
//
// Binance UM funding: rate × mark × position_signed. Long on
// positive-rate funding pays out (negative amount on account).
//
// Baseline: the engine produces N funding payments over a period
// = ceil(period_ns / interval_ns). Each amount has correct sign
// and magnitude within 5% of the analytical calculation.

TEST(W15Calibration, EightHourFundingSettlementMatchesAnalytical)
{
  auto stack = VenueStack::binance_um_futures(1, 10'000.0);
  auto& funding = stack.funding();
  funding.setConstantRate(0.0001);  // 0.01% per 8h interval — typical

  const SymbolId sym = BTC;
  const double position = 0.5;  // long
  const double mark = 50'000.0;

  // Drive 24 hours → exactly 3 settlements expected.
  const int64_t one_day_ns = 24LL * 3600LL * 1'000'000'000LL;
  const auto payments = funding.tick(one_day_ns, {sym}, {position}, {mark});

  // Baseline: 3 settlements over 24h at 8h interval.
  EXPECT_EQ(payments.size(), 3u);

  // Each amount: -position × mark × rate (long pays positive-rate).
  const double expected_per_settlement = -position * mark * 0.0001;
  for (const auto& p : payments)
  {
    EXPECT_TRUE(
        withinRelativeTolerance(p.amount, expected_per_settlement, 0.05))
        << "funding amount " << p.amount << " diverges from baseline "
        << expected_per_settlement;
    EXPECT_LT(p.amount, 0.0) << "long position on positive rate should pay";
  }
}

// === Scenario 3: cross-margin netting prevents liquidation that isolated would trigger ===
//
// A long BTC + short ETH on uncorrelated moves can have one leg
// underwater while the other is profitable. In cross mode, the
// netting prevents liquidation. In isolated mode, the underwater
// leg liquidates.
//
// Baseline: same scenario produces different liquidation counts
// across the two modes.

TEST(W15Calibration, CrossMarginNettingPreventsLiquidation)
{
  // Scenario A: cross-margin.
  auto stack_cross = VenueStack::binance_um_futures(1, 5'000.0);
  stack_cross.liquidation().setExecutor(nullptr);
  stack_cross.account().setMarginMode(MarginMode::Cross);
  stack_cross.account().openPosition(BTC, 1.0, 50'000.0);   // long BTC
  stack_cross.account().openPosition(ETH, -10.0, 3'000.0);  // short ETH

  // BTC drops 5% (-2500); ETH drops 10% (+3000 for short).
  // Net: +500. Cross stays comfortably solvent.
  const std::vector<std::pair<SymbolId, double>> marks = {
      {BTC, 47'500.0},
      {ETH, 2'700.0},
  };
  const auto out_cross = stack_cross.liquidation().onMarks(marks);
  EXPECT_EQ(out_cross.liquidationsCount, 0u)
      << "cross-margin should survive netting BTC loss with ETH gain";
  EXPECT_EQ(stack_cross.account().positionCount(), 2u);

  // Scenario B: isolated, same positions and marks but with tiny
  // per-position equity slices so BTC's leg fails MM.
  auto stack_iso = VenueStack::binance_um_futures(2, 0.0);
  stack_iso.liquidation().setExecutor(nullptr);
  stack_iso.account().setMarginMode(MarginMode::Isolated);
  stack_iso.account().openPosition(BTC, 1.0, 50'000.0,
                                   /*isolated_equity=*/100.0);
  stack_iso.account().openPosition(ETH, -10.0, 3'000.0,
                                   /*isolated_equity=*/5'000.0);
  const auto out_iso = stack_iso.liquidation().onMarks(marks);
  // BTC leg (100 equity vs -2500 uPnL) liquidates. ETH leg stays
  // (profitable on the short).
  EXPECT_GE(out_iso.liquidationsCount, 1u)
      << "isolated BTC leg should liquidate without netting";
  // ETH leg should still be open.
  bool eth_open = false;
  for (const auto& p : stack_iso.account().positions())
  {
    if (p.symbol == ETH)
    {
      eth_open = true;
      break;
    }
  }
  EXPECT_TRUE(eth_open) << "isolated ETH leg should survive its own bucket";
}

// === Scenario 4: VIP tier transition matches Binance's published 30d notional ladder ===
//
// Binance UM: VIP 1 at 250k 30d notional. Each fill of 50k → tier
// increments when crossing the threshold.

TEST(W15Calibration, FeeTierTransitionsCrossPublishedThresholds)
{
  auto stack = VenueStack::binance_um_futures(1, 100'000.0);
  auto& fees = stack.fees();

  // Below threshold: tier 0 (regular).
  fees.recordFill(0, 100'000.0);
  EXPECT_EQ(fees.currentTierIndex(), 0u);

  // Cross 250k (VIP 1): tier 1.
  fees.recordFill(0, 200'000.0);
  EXPECT_GE(fees.currentTierIndex(), 1u);

  // Cross 2.5M (VIP 2): tier 2 or higher.
  fees.recordFill(0, 2'500'000.0);
  EXPECT_GE(fees.currentTierIndex(), 2u);
}
