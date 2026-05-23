/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/liquidation_engine.h"

#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;

LeveragedPosition pos(uint64_t acct, double qty, double entry, double equity)
{
  return LeveragedPosition{.accountId = acct, .symbol = BTC, .quantity = qty, .entryPrice = entry, .equity = equity};
}
}  // namespace

TEST(LiquidationEngine, MmFractionResolvesByTier)
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.addTier(100'000.0, 0.01);
  e.addTier(1'000'000.0, 0.025);

  EXPECT_DOUBLE_EQ(e.mmFractionFor(50'000), 0.005);
  EXPECT_DOUBLE_EQ(e.mmFractionFor(200'000), 0.01);
  EXPECT_DOUBLE_EQ(e.mmFractionFor(5'000'000), 0.025);
}

TEST(LiquidationEngine, UnderwaterLongLiquidates)
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  // 10 BTC long entered at 100; equity 10. Notional at 80 is 800,
  // MM = 4. PnL = 10 * (80 - 100) = -200; net = -190 < 4 → liquidate.
  e.openPosition(pos(/*acct=*/1, /*qty=*/10.0, /*entry=*/100.0, /*equity=*/10.0));

  const auto out = e.onMark(BTC, 80.0);
  ASSERT_EQ(out.liquidationsCount, 1u);
  EXPECT_EQ(out.liquidated.front(), 1u);
  EXPECT_TRUE(e.positions().empty());
}

TEST(LiquidationEngine, HealthyPositionUntouched)
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  // 1 BTC long at 100, equity 50. PnL at 99 = -1; net 49 vs MM 0.495 → safe.
  e.openPosition(pos(1, 1.0, 100.0, 50.0));

  const auto out = e.onMark(BTC, 99.0);
  EXPECT_EQ(out.liquidationsCount, 0u);
  EXPECT_EQ(e.positions().size(), 1u);
}

TEST(LiquidationEngine, InsuranceFundAbsorbsDeficit)
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(1000.0);
  e.setAdlEnabled(false);
  e.setLiquidationSlippageBps(0.0);  // isolate slippage from PnL math
  // Long 10 BTC at 100, equity 50; price drops to 40. PnL = -600;
  // residual equity = -550 → deficit 550 paid from fund.
  e.openPosition(pos(1, 10.0, 100.0, 50.0));

  const auto out = e.onMark(BTC, 40.0);
  EXPECT_EQ(out.liquidationsCount, 1u);
  EXPECT_NEAR(out.insuranceFundDelta, -550.0, 1e-6);
  EXPECT_NEAR(e.insuranceFundBalance(), 450.0, 1e-6);
  EXPECT_EQ(out.adlCloseoutsCount, 0u);
}

TEST(LiquidationEngine, AdlClosesProfitableOppositeWhenFundDepleted)
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(0.0);  // no fund to absorb
  e.setAdlEnabled(true);
  e.setLiquidationSlippageBps(0.0);
  // Underwater long: 10 BTC at 100, equity 50; mark drops to 40. PnL=-600 → deficit 550.
  e.openPosition(pos(/*acct=*/1, 10.0, 100.0, 50.0));
  // Profitable short: -5 BTC at 100; at mark 40 PnL = -5*(40-100) = +300.
  e.openPosition(pos(/*acct=*/2, -5.0, 100.0, 100.0));
  // Larger profitable short: -10 BTC at 100; PnL = +600.
  e.openPosition(pos(/*acct=*/3, -10.0, 100.0, 100.0));

  const auto out = e.onMark(BTC, 40.0);
  EXPECT_EQ(out.liquidationsCount, 1u);
  // ADL ranks by PnL ratio (upnl/equity): acct 2 ratio = 3, acct 3 ratio = 6.
  // acct 3 alone covers 550. So just one closeout.
  EXPECT_EQ(out.adlCloseoutsCount, 1u);
  EXPECT_EQ(out.adlClosedOut.front(), 3u);
}

TEST(LiquidationEngine, AdlDisabledLeavesDeficitUnpaid)
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(0.0);
  e.setAdlEnabled(false);
  e.setLiquidationSlippageBps(0.0);
  e.openPosition(pos(1, 10.0, 100.0, 50.0));
  e.openPosition(pos(2, -5.0, 100.0, 100.0));  // profitable but ADL off

  const auto out = e.onMark(BTC, 40.0);
  EXPECT_EQ(out.liquidationsCount, 1u);
  EXPECT_EQ(out.adlCloseoutsCount, 0u);
  // The profitable short stays in the book.
  ASSERT_EQ(e.positions().size(), 1u);
  EXPECT_EQ(e.positions().front().accountId, 2u);
}

TEST(LiquidationEngine, MmTierEscalatesOnNotional)
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.addTier(50'000.0, 0.05);  // 10x higher MM in this bracket

  // Small position: 1 BTC long at 100, equity 1; mark 99.
  // Notional 99 → tier 0 (MM 0.5%, req = 0.495); equity+upnl = 0
  // → liquidate.
  e.openPosition(pos(1, 1.0, 100.0, 1.0));
  // Large position: 1000 BTC long at 100, equity 1000; mark 99.
  // Notional 99000 → tier 1 (MM 5%, req = 4950); equity+upnl = 0
  // → liquidate.
  e.openPosition(pos(2, 1000.0, 100.0, 1000.0));

  const auto out = e.onMark(BTC, 99.0);
  EXPECT_EQ(out.liquidationsCount, 2u);
}

TEST(LiquidationEngine, CannedProfileBinanceUmFutures)
{
  auto e = LiquidationEngine::binance_um_futures();
  EXPECT_GE(e.tiers().size(), 5u);
  EXPECT_GT(e.insuranceFundBalance(), 0.0);
  EXPECT_TRUE(e.adlEnabled());
}

// === T036: executor-routed liquidation ===

namespace
{
// Helper: feed a book snapshot through the executor so resting
// liquidity exists for liquidation market orders to walk.
void feedBook(SimulatedExecutor& ex, SymbolId sym,
              const std::vector<std::pair<double, double>>& bids,
              const std::vector<std::pair<double, double>>& asks)
{
  std::pmr::monotonic_buffer_resource pool(1024);
  std::pmr::vector<BookLevel> b(&pool);
  std::pmr::vector<BookLevel> a(&pool);
  for (const auto& [px, qty] : bids)
  {
    b.emplace_back(Price::fromDouble(px), Quantity::fromDouble(qty));
  }
  for (const auto& [px, qty] : asks)
  {
    a.emplace_back(Price::fromDouble(px), Quantity::fromDouble(qty));
  }
  ex.onBookUpdate(sym, b, a);
}
}  // namespace

TEST(LiquidationEngine, ExecutorRoutedClosesAgainstBook)
{
  // Engine attached to executor; populated book → liquidation should
  // fill at the bid (closing a long) without using flat slippage.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setExecutor(&ex);

  feedBook(ex, BTC, {{80.0, 100.0}}, {{81.0, 100.0}});

  // 10 BTC long at 100, equity 10 → underwater at 80.
  e.openPosition(pos(1, 10.0, 100.0, 10.0));

  const auto out = e.onMark(BTC, 80.0);
  EXPECT_EQ(out.liquidationsCount, 1u);
  EXPECT_EQ(out.liquidated.front(), 1u);
  EXPECT_TRUE(e.positions().empty());
}

// Note: the simulator's market-order matching currently fills at
// full quantity at the best-level price (no book walk / no
// depth-cap). When that limitation is closed (filed as a follow-up
// because it's an Executor concern, not a LiquidationEngine one),
// the engine's partial-fill rollover path — already exercised
// implicitly here via the closeThroughExecutor() return value — will
// produce partial liquidations on thin books that roll to the next
// tick.

TEST(LiquidationEngine, ExecutorRoutedEmptyBookSkipsLiquidation)
{
  // Empty book: executor can't fill. Position stays.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setExecutor(&ex);

  // No book fed.
  e.openPosition(pos(1, 10.0, 100.0, 10.0));

  const auto out = e.onMark(BTC, 80.0);
  EXPECT_EQ(out.liquidationsCount, 0u);
  ASSERT_EQ(e.positions().size(), 1u);
}

TEST(LiquidationEngine, DetachedExecutorPreservesFlatBpsBehaviour)
{
  // Default (no executor) → existing T032 semantics.
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(1000.0);
  e.setAdlEnabled(false);
  e.setLiquidationSlippageBps(0.0);
  e.openPosition(pos(1, 10.0, 100.0, 50.0));

  const auto out = e.onMark(BTC, 40.0);
  EXPECT_EQ(out.liquidationsCount, 1u);
  EXPECT_NEAR(out.insuranceFundDelta, -550.0, 1e-6);
}
