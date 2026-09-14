/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The pre-trade gate against the state the snapshot reports.
//
// The two disagreed in four ways, and every one of them was silent: a
// single-strategy desk had every opening order refused while snapshot()
// showed a clean book; a normal flattening by one of two strategies armed the
// kill switch on the other; risk-reducing orders were refused by the state
// limits, which is exactly when you need them; and a drawdown cap without a
// capital base did nothing at all.

#include "flox/risk/portfolio_risk.h"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace flox::risk;

namespace
{

StrategyAccount gross(double value)
{
  StrategyAccount a;
  a.gross_exposure = value;
  return a;
}

StrategyAccount pnl(double realized)
{
  StrategyAccount a;
  a.realized_pnl = realized;
  return a;
}

}  // namespace

// A desk running one strategy holds 100% of its own book by definition. The
// gate refused every opening order it ever sent, at the concentration cap
// from the documented quick-start config, and the snapshot the operator was
// looking at reported no breach and no accounts.
TEST(PortfolioRiskGate, SingleStrategyIsNotConcentrationRisk)
{
  RiskRules rules;
  rules.max_concentration_pct = 0.40;
  PortfolioRiskAggregator agg(rules, 100'000.0);

  EXPECT_FALSE(agg.checkOrder("solo", 10'000.0, "buy").has_value());

  agg.update("solo", gross(5'000.0), field_mask::GROSS_EXPOSURE);
  EXPECT_FALSE(agg.checkOrder("solo", 10'000.0, "buy").has_value());

  const auto snap = agg.snapshot();
  EXPECT_FALSE(snap.kill_switch_active);
  EXPECT_TRUE(snap.active_breaches.empty());
}

// Two strategies, one of them holding no exposure. Counting registered rows
// rather than contributing ones made the other strategy "100% of gross" the
// moment it took its first position.
TEST(PortfolioRiskGate, IdleRowIsNotAContributor)
{
  RiskRules rules;
  rules.max_concentration_pct = 0.40;
  PortfolioRiskAggregator agg(rules, 100'000.0);

  StrategyAccount idle;
  idle.trade_count = 0;
  agg.update("beta", idle, field_mask::TRADE_COUNT);
  agg.update("alpha", gross(1'000.0), field_mask::GROSS_EXPOSURE);

  const auto snap = agg.snapshot();
  EXPECT_FALSE(snap.kill_switch_active);
  EXPECT_TRUE(snap.active_breaches.empty());
  EXPECT_FALSE(agg.checkOrder("alpha", 100.0, "buy").has_value());
}

// The scenario the audit called routine: a balanced pair where one side
// closes out and goes flat. Trading for the whole portfolio used to halt
// there, clearable only by hand and armed again on the next update.
TEST(PortfolioRiskGate, FlatteningOneOfTwoStrategiesDoesNotHaltThePortfolio)
{
  RiskRules rules;
  rules.max_concentration_pct = 0.60;
  PortfolioRiskAggregator agg(rules, 100'000.0);

  agg.update("a", gross(10'000.0), field_mask::GROSS_EXPOSURE);
  agg.update("b", gross(10'000.0), field_mask::GROSS_EXPOSURE);
  ASSERT_FALSE(agg.snapshot().kill_switch_active);

  agg.update("b", gross(0.0), field_mask::GROSS_EXPOSURE);

  const auto snap = agg.snapshot();
  EXPECT_FALSE(snap.kill_switch_active);
  EXPECT_TRUE(snap.active_breaches.empty());
  EXPECT_FALSE(agg.checkOrder("a", 500.0, "buy").has_value());
}

// Concentration still fires when two strategies really are lopsided.
TEST(PortfolioRiskGate, ConcentrationStillFiresWithTwoContributors)
{
  RiskRules rules;
  rules.max_concentration_pct = 0.60;
  PortfolioRiskAggregator agg(rules, 100'000.0);

  agg.update("a", gross(8'000.0), field_mask::GROSS_EXPOSURE);
  agg.update("b", gross(2'000.0), field_mask::GROSS_EXPOSURE);

  const auto snap = agg.snapshot();
  EXPECT_TRUE(snap.kill_switch_active);
  ASSERT_FALSE(snap.active_breaches.empty());
  EXPECT_EQ(snap.active_breaches.front().rule, "max_concentration_pct");
}

// The gate's comparison has to be the breach list's comparison. There used to
// be a window exactly one unit in the last place wide, at the limit, where
// orders were refused and the snapshot showed nothing to explain it.
TEST(PortfolioRiskGate, DailyLossBoundaryMatchesTheBreachList)
{
  RiskRules rules;
  rules.max_daily_loss = 10'000.0;
  PortfolioRiskAggregator agg(rules, 100'000.0);

  agg.update("a", pnl(-10'000.0), field_mask::REALIZED_PNL);
  EXPECT_TRUE(agg.snapshot().active_breaches.empty());
  EXPECT_FALSE(agg.checkOrder("a", 100.0, "buy").has_value());

  agg.update("a", pnl(-10'000.01), field_mask::REALIZED_PNL);
  EXPECT_FALSE(agg.snapshot().active_breaches.empty());
  EXPECT_TRUE(agg.checkOrder("a", 100.0, "buy").has_value());
}

// The sign of the cap carries no meaning; the breach list has always read it
// as a magnitude. Read literally by the gate, a negative cap refused every
// order from a flat start and only released after 10,001 of profit, which is
// not reachable without trading.
TEST(PortfolioRiskGate, NegativeDailyLossCapIsAMagnitude)
{
  RiskRules rules;
  rules.max_daily_loss = -10'000.0;
  PortfolioRiskAggregator agg(rules, 100'000.0);

  agg.update("a", pnl(0.0), field_mask::REALIZED_PNL);
  EXPECT_FALSE(agg.checkOrder("a", 100.0, "buy").has_value());

  agg.update("a", pnl(5'000.0), field_mask::REALIZED_PNL);
  EXPECT_FALSE(agg.checkOrder("a", 100.0, "buy").has_value());

  agg.update("a", pnl(-10'001.0), field_mask::REALIZED_PNL);
  EXPECT_TRUE(agg.checkOrder("a", 100.0, "buy").has_value());
}

// A reduce passes every state limit right up to the point where the kill
// switch takes over, which is exactly the stretch where a desk needs to be
// able to get out. Daily loss at the cap, gross a hair under it and
// concentration just inside its cap: a buy is refused, the matching reduce is
// not.
TEST(PortfolioRiskGate, ReduceOrdersPassTheStateLimits)
{
  RiskRules rules;
  rules.max_daily_loss = 10'000.0;
  rules.max_gross_exposure = 50'000.0;
  rules.max_concentration_pct = 0.99;
  PortfolioRiskAggregator agg(rules, 100'000.0);

  StrategyAccount deep;
  deep.realized_pnl = -10'000.0;
  deep.gross_exposure = 49'000.0;
  agg.update("a", deep, field_mask::REALIZED_PNL | field_mask::GROSS_EXPOSURE);
  agg.update("b", gross(1'000.0), field_mask::GROSS_EXPOSURE);

  ASSERT_FALSE(agg.snapshot().kill_switch_active);
  EXPECT_TRUE(agg.checkOrder("a", 10'000.0, "buy").has_value());
  EXPECT_FALSE(agg.checkOrder("a", 10'000.0, "reduce").has_value());
}

// A caller who reports only gross exposure -- the shape of every example in
// the docs -- still gets its sells recognised as risk-reducing. Reading an
// unreported net as zero made every order look like it increased exposure.
TEST(PortfolioRiskGate, SellReducesWhenOnlyGrossIsReported)
{
  RiskRules rules;
  rules.max_gross_exposure = 50'000.0;
  PortfolioRiskAggregator agg(rules, 100'000.0);

  agg.update("a", gross(49'000.0), field_mask::GROSS_EXPOSURE);

  EXPECT_FALSE(agg.checkOrder("a", 10'000.0, "sell").has_value());
  EXPECT_TRUE(agg.checkOrder("a", 10'000.0, "buy").has_value());
}

// A drawdown cap needs something to measure against. With the default
// initial equity of zero the rule was dead: a 500,000 loss reported 0.0000
// and let every order through.
TEST(PortfolioRiskGate, DrawdownRuleRefusesToStartWithoutCapital)
{
  RiskRules rules;
  rules.max_drawdown_pct = 0.20;

  EXPECT_THROW(PortfolioRiskAggregator(rules, 0.0), std::invalid_argument);
  EXPECT_THROW(PortfolioRiskAggregator(rules, -1.0), std::invalid_argument);
  EXPECT_NO_THROW(PortfolioRiskAggregator(rules, 100'000.0));

  RiskRules noDrawdown;
  noDrawdown.max_gross_exposure = 1'000.0;
  EXPECT_NO_THROW(PortfolioRiskAggregator(noDrawdown, 0.0));
}

// Drawdown is a fraction and cannot exceed one. A small positive peak against
// a large loss used to print 500001.0 -- "50,000,100%" -- into the snapshot,
// into the breach text and into every report downstream.
TEST(PortfolioRiskGate, DrawdownIsClampedToOne)
{
  RiskRules rules;
  rules.max_drawdown_pct = 0.20;
  PortfolioRiskAggregator agg(rules, 1.0);

  agg.update("a", pnl(1'000.0), field_mask::REALIZED_PNL);
  agg.update("a", pnl(-500'000.0), field_mask::REALIZED_PNL);

  const auto snap = agg.snapshot();
  EXPECT_LE(snap.drawdown_pct, 1.0);
  EXPECT_GT(snap.drawdown_pct, 0.99);
}
