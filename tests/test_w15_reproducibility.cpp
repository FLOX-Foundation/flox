/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// T065: W15 venue-stack reproducibility audit.
//
// 28 PRs across round 4 + 5 introduced potential non-determinism
// sources: iceberg refresh jitter, cross-margin candidate gathering,
// cascade impact loops, ADL candidate pools. This test pins
// determinism end-to-end: build two identical VenueStacks, drive
// the same synthetic tape through both, and assert every captured
// engine stat is bit-identical.
//
// A failure here is the worst class of backtest bug — results look
// plausible, decisions get made, then re-runs produce different
// numbers and the researcher discovers they can't trust any prior
// result. This test catches the regression at PR time.

#include "flox/backtest/account.h"
#include "flox/backtest/fee_schedule.h"
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

// Drive a healthy multi-symbol tape through `stack`. Returns a
// stat vector that captures every observable engine state we want
// reproducible across runs.
struct StackSnapshot
{
  double equity{0.0};
  size_t position_count{0};
  uint64_t liquidations_count{0};
  uint64_t adl_closeouts_count{0};
  uint64_t insurance_payments_count{0};
  double insurance_fund_balance{0.0};
  double rolling_notional_30d{0.0};
  size_t fee_tier_index{0};
  uint64_t ticks_to_first_adl{0};
  std::vector<uint32_t> cascade_sizes;
  std::vector<double> deficits_paid_by_fund;
  std::vector<double> deficits_paid_by_adl;
  std::vector<double> fund_balance_history;

  bool operator==(const StackSnapshot& o) const
  {
    return equity == o.equity && position_count == o.position_count &&
           liquidations_count == o.liquidations_count &&
           adl_closeouts_count == o.adl_closeouts_count &&
           insurance_payments_count == o.insurance_payments_count &&
           insurance_fund_balance == o.insurance_fund_balance &&
           rolling_notional_30d == o.rolling_notional_30d &&
           fee_tier_index == o.fee_tier_index &&
           ticks_to_first_adl == o.ticks_to_first_adl &&
           cascade_sizes == o.cascade_sizes &&
           deficits_paid_by_fund == o.deficits_paid_by_fund &&
           deficits_paid_by_adl == o.deficits_paid_by_adl &&
           fund_balance_history == o.fund_balance_history;
  }
};

StackSnapshot capture(VenueStack& stack)
{
  StackSnapshot s;
  s.equity = stack.account().equity();
  s.position_count = stack.account().positionCount();
  s.liquidations_count = stack.liquidation().liquidationsCount();
  s.adl_closeouts_count = stack.liquidation().adlCloseoutsCount();
  s.insurance_payments_count = stack.liquidation().insurancePaymentsCount();
  s.insurance_fund_balance = stack.liquidation().insuranceFundBalance();
  s.rolling_notional_30d = stack.account().rollingNotional30d();
  s.fee_tier_index = stack.fees().currentTierIndex();
  s.ticks_to_first_adl = stack.liquidation().ticksToFirstAdl();
  s.cascade_sizes = stack.liquidation().cascadeSizesPerTick();
  s.deficits_paid_by_fund = stack.liquidation().deficitsPaidByFund();
  s.deficits_paid_by_adl = stack.liquidation().deficitsPaidByAdl();
  s.fund_balance_history = stack.liquidation().fundBalanceHistory();
  return s;
}

void driveSyntheticTape(VenueStack& stack, int ticks, bool cascade_scenario)
{
  auto& acct = stack.account();
  auto& liq = stack.liquidation();
  auto& fees = stack.fees();

  if (cascade_scenario)
  {
    // Underwater scenario: trigger liquidations + ADL + insurance fund.
    // Detach the executor so the engine uses flat-bps slippage fallback
    // (otherwise market-order submits against an unfed book never fill
    // and the cross-walk stops without recording a liquidation).
    liq.setExecutor(nullptr);
    Account aOpposite(99, 5'000.0);
    aOpposite.openPosition(BTC, -1.0, 50'000.0);
    liq.attachAccount(&aOpposite);
    acct.openPosition(BTC, 1.0, 50'000.0);
    for (int i = 0; i < ticks; ++i)
    {
      const int64_t ts = static_cast<int64_t>(i) * 60 * 1'000'000'000LL;
      // Deterministic price walk descending.
      const double price = 50'000.0 - 200.0 * i;
      const std::vector<std::pair<SymbolId, double>> marks = {{BTC, price}};
      (void)liq.onMarks(marks, ts);
      fees.recordFill(ts, 25'000.0);
    }
    liq.detachAccount(aOpposite.accountId());
  }
  else
  {
    // Healthy multi-symbol walk. No liquidations expected.
    acct.openPosition(BTC, 0.1, 50'000.0);
    acct.openPosition(ETH, -2.0, 3'000.0);
    for (int i = 0; i < ticks; ++i)
    {
      const int64_t ts = static_cast<int64_t>(i) * 60 * 1'000'000'000LL;
      const std::vector<std::pair<SymbolId, double>> marks = {
          {BTC, 50'000.0 + 10.0 * i},
          {ETH, 3'000.0 - 1.0 * i},
      };
      (void)liq.onMarks(marks, ts);
      if (i % 5 == 0)
      {
        fees.recordFill(ts, 50'000.0);
      }
    }
  }
}
}  // namespace

TEST(W15Reproducibility, HealthyWalkBitIdenticalAcrossRuns)
{
  auto stack_a = VenueStack::binance_um_futures(42, 10'000.0);
  auto stack_b = VenueStack::binance_um_futures(42, 10'000.0);
  driveSyntheticTape(stack_a, /*ticks=*/100, /*cascade=*/false);
  driveSyntheticTape(stack_b, /*ticks=*/100, /*cascade=*/false);

  const auto sa = capture(stack_a);
  const auto sb = capture(stack_b);
  EXPECT_TRUE(sa == sb)
      << "venue stack produced divergent state across identical runs";
}

TEST(W15Reproducibility, CascadeScenarioBitIdenticalAcrossRuns)
{
  auto stack_a = VenueStack::binance_um_futures(7, 1'000.0);
  auto stack_b = VenueStack::binance_um_futures(7, 1'000.0);
  driveSyntheticTape(stack_a, /*ticks=*/30, /*cascade=*/true);
  driveSyntheticTape(stack_b, /*ticks=*/30, /*cascade=*/true);

  const auto sa = capture(stack_a);
  const auto sb = capture(stack_b);
  EXPECT_TRUE(sa == sb)
      << "cascade scenario produced divergent state across identical runs";
  // Sanity: the scenario actually triggered some liquidation work,
  // otherwise the test is vacuous.
  EXPECT_GT(sa.liquidations_count, 0u);
}

TEST(W15Reproducibility, MultipleVenuesEachReproducible)
{
  // Repeat the bit-identical check across every venue factory.
  auto runOnce = [](VenueStack stack)
  {
    driveSyntheticTape(stack, 50, false);
    return capture(stack);
  };
  EXPECT_TRUE(runOnce(VenueStack::binance_um_futures(1, 5'000.0)) ==
              runOnce(VenueStack::binance_um_futures(1, 5'000.0)));
  EXPECT_TRUE(runOnce(VenueStack::bybit_linear(1, 5'000.0)) ==
              runOnce(VenueStack::bybit_linear(1, 5'000.0)));
  EXPECT_TRUE(runOnce(VenueStack::okx_swap(1, 5'000.0)) ==
              runOnce(VenueStack::okx_swap(1, 5'000.0)));
  EXPECT_TRUE(runOnce(VenueStack::deribit(1, 5'000.0)) ==
              runOnce(VenueStack::deribit(1, 5'000.0)));
}
