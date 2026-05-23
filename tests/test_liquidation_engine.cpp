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

// === T045: ADL ranking variants ===

TEST(LiquidationEngine, AdlRankingDefaultsToPnlRatio)
{
  LiquidationEngine e;
  EXPECT_EQ(e.adlRanking(), AdlRanking::PnlRatio);
}

TEST(LiquidationEngine, AdlRankingByNameAcceptsKnownStrings)
{
  LiquidationEngine e;
  e.setAdlRankingByName("binance");
  EXPECT_EQ(e.adlRanking(), AdlRanking::Binance);
  e.setAdlRankingByName("BYBIT");
  EXPECT_EQ(e.adlRanking(), AdlRanking::Bybit);
  e.setAdlRankingByName("position_size");
  EXPECT_EQ(e.adlRanking(), AdlRanking::PositionSize);
  e.setAdlRankingByName("pnl_ratio");
  EXPECT_EQ(e.adlRanking(), AdlRanking::PnlRatio);
  // Unknown name leaves prior ranking in place.
  e.setAdlRankingByName("not-a-real-mode");
  EXPECT_EQ(e.adlRanking(), AdlRanking::PnlRatio);
}

TEST(LiquidationEngine, AdlRankingBinancePicksHigherNotionalFirst)
{
  // Both A and B are profitable opposite-side; deficit fits inside
  // either one alone. PnlRatio picks A (higher upnl/equity).
  // Binance picks B (higher upnl × leverage_via_notional).
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(0.0);
  e.setAdlEnabled(true);
  e.setLiquidationSlippageBps(0.0);

  // Underwater long: 5 BTC at 100, equity 50; mark 40
  // → realized = -300, residual = -250, deficit = 250.
  e.openPosition(pos(/*acct=*/1, 5.0, 100.0, 50.0));
  // Profitable A: short -1 BTC at 100, equity 10. At mark 40
  // → upnl = +60. PnlRatio = 60/10 = 6.0. Binance = 60 × (40/10) = 240.
  e.openPosition(pos(/*acct=*/2, -1.0, 100.0, 10.0));
  // Profitable B: short -5 BTC at 100, equity 100. At mark 40
  // → upnl = +300. PnlRatio = 300/100 = 3.0. Binance = 300 × (200/100) = 600.
  e.openPosition(pos(/*acct=*/3, -5.0, 100.0, 100.0));

  // Default ranking (PnlRatio): A comes first (60), needs 190 more
  // → also closes B. Two closeouts.
  {
    auto e1 = e;
    const auto out = e1.onMark(BTC, 40.0);
    EXPECT_EQ(out.liquidationsCount, 1u);
    EXPECT_EQ(out.adlCloseoutsCount, 2u);
  }

  // Binance ranking: B alone (300) covers 250. Single closeout, acct=3.
  {
    auto e2 = e;
    e2.setAdlRanking(AdlRanking::Binance);
    const auto out = e2.onMark(BTC, 40.0);
    EXPECT_EQ(out.liquidationsCount, 1u);
    EXPECT_EQ(out.adlCloseoutsCount, 1u);
    EXPECT_EQ(out.adlClosedOut.front(), 3u);
  }

  // Bybit (alias of Binance): same behaviour.
  {
    auto e3 = e;
    e3.setAdlRanking(AdlRanking::Bybit);
    const auto out = e3.onMark(BTC, 40.0);
    EXPECT_EQ(out.adlCloseoutsCount, 1u);
    EXPECT_EQ(out.adlClosedOut.front(), 3u);
  }
}

TEST(LiquidationEngine, AdlRankingPositionSizeIgnoresPnlRatio)
{
  // Deficit small enough that either A or B alone closes it.
  // PnlRatio picks A (small position, large ratio).
  // PositionSize picks B (large position, tiny ratio).
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(0.0);
  e.setAdlEnabled(true);
  e.setLiquidationSlippageBps(0.0);

  // Underwater long: 2 BTC at 100, equity 20; mark 40
  // → realized = -120, residual = -100, deficit = 100.
  e.openPosition(pos(/*acct=*/1, 2.0, 100.0, 20.0));
  // A: short -3 BTC at 100, equity 30. upnl=+180, ratio=6, size=3.
  e.openPosition(pos(/*acct=*/2, -3.0, 100.0, 30.0));
  // B: short -10 BTC at 50, equity 500. upnl=+100, ratio=0.2, size=10.
  e.openPosition(pos(/*acct=*/3, -10.0, 50.0, 500.0));

  {
    auto e1 = e;
    e1.setAdlRanking(AdlRanking::PnlRatio);
    const auto out = e1.onMark(BTC, 40.0);
    EXPECT_EQ(out.adlCloseoutsCount, 1u);
    EXPECT_EQ(out.adlClosedOut.front(), 2u);
  }
  {
    auto e2 = e;
    e2.setAdlRanking(AdlRanking::PositionSize);
    const auto out = e2.onMark(BTC, 40.0);
    EXPECT_EQ(out.adlCloseoutsCount, 1u);
    EXPECT_EQ(out.adlClosedOut.front(), 3u);
  }
}

TEST(LiquidationEngine, BinancePresetUsesBinanceAdlRanking)
{
  auto e = LiquidationEngine::binance_um_futures();
  EXPECT_EQ(e.adlRanking(), AdlRanking::Binance);
}

TEST(LiquidationEngine, BybitPresetUsesBybitAdlRanking)
{
  auto e = LiquidationEngine::bybit_linear();
  EXPECT_EQ(e.adlRanking(), AdlRanking::Bybit);
}

// === T039: cascade statistics ===

TEST(LiquidationEngine, StatsEmptyOnFreshEngine)
{
  LiquidationEngine e;
  EXPECT_TRUE(e.deficitsPaidByFund().empty());
  EXPECT_TRUE(e.deficitsPaidByAdl().empty());
  EXPECT_TRUE(e.cascadeSizesPerTick().empty());
  EXPECT_TRUE(e.fundBalanceHistory().empty());
  EXPECT_EQ(e.ticksToFirstAdl(), UINT64_MAX);
}

TEST(LiquidationEngine, StatsSingleLiquidationNoAdl)
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(1000.0);
  e.setAdlEnabled(false);
  e.setLiquidationSlippageBps(0.0);
  e.openPosition(pos(1, 10.0, 100.0, 50.0));

  e.onMark(BTC, 40.0);
  // 1 liquidation, fund absorbed 550 deficit, no ADL.
  ASSERT_EQ(e.cascadeSizesPerTick().size(), 1u);
  EXPECT_EQ(e.cascadeSizesPerTick().front(), 1u);
  ASSERT_EQ(e.deficitsPaidByFund().size(), 1u);
  EXPECT_NEAR(e.deficitsPaidByFund().front(), 550.0, 1e-6);
  EXPECT_TRUE(e.deficitsPaidByAdl().empty());
  EXPECT_EQ(e.ticksToFirstAdl(), UINT64_MAX);
  ASSERT_EQ(e.fundBalanceHistory().size(), 1u);
  EXPECT_NEAR(e.fundBalanceHistory().front(), 450.0, 1e-6);
}

TEST(LiquidationEngine, StatsCascadeTriggersAdlAndRecordsTickIdx)
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(0.0);
  e.setAdlEnabled(true);
  e.setLiquidationSlippageBps(0.0);

  // 10 BTC long at 100, equity 300 → safe at mark=99, underwater at 40.
  e.openPosition(pos(1, 10.0, 100.0, 300.0));
  // Profitable short at 40.
  e.openPosition(pos(2, -10.0, 100.0, 100.0));

  // Tick 0: both healthy at mark=99. No liquidations.
  e.onMark(BTC, 99.0);
  EXPECT_EQ(e.ticksToFirstAdl(), UINT64_MAX);
  EXPECT_TRUE(e.cascadeSizesPerTick().empty());

  // Tick 1: long underwater → liquidate, deficit 300, ADL fires.
  e.onMark(BTC, 40.0);
  EXPECT_EQ(e.ticksToFirstAdl(), 1u);
  EXPECT_FALSE(e.deficitsPaidByAdl().empty());
}

TEST(LiquidationEngine, StatsResetClears)
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(1000.0);
  e.setAdlEnabled(false);
  e.setLiquidationSlippageBps(0.0);
  e.openPosition(pos(1, 10.0, 100.0, 50.0));
  e.onMark(BTC, 40.0);
  EXPECT_FALSE(e.cascadeSizesPerTick().empty());

  e.resetStats();
  EXPECT_TRUE(e.cascadeSizesPerTick().empty());
  EXPECT_TRUE(e.deficitsPaidByFund().empty());
  EXPECT_EQ(e.liquidationsCount(), 0u);
  EXPECT_EQ(e.insurancePaymentsCount(), 0u);
  EXPECT_EQ(e.ticksToFirstAdl(), UINT64_MAX);
}

// === T038: mark-impact feedback + intra-tick cascades ===

TEST(LiquidationEngine, MarkImpactDefaultsToNoneNoCascade)
{
  // Without an impact model, a single onMark call performs exactly
  // one liquidation pass even if the book mid would have moved.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setExecutor(&ex);
  EXPECT_EQ(e.markImpactModel(), LiquidationEngine::MarkImpactModel::None);

  feedBook(ex, BTC, {{80.0, 100.0}}, {{81.0, 100.0}});
  e.openPosition(pos(1, 10.0, 100.0, 10.0));
  const auto out = e.onMark(BTC, 80.0);
  // Single pass: cascade list has exactly one entry.
  EXPECT_EQ(out.liquidationsCount, 1u);
  EXPECT_EQ(e.cascadeSizesPerTick().size(), 1u);
}

TEST(LiquidationEngine, MarkImpactByNameAcceptsKnownStrings)
{
  LiquidationEngine e;
  e.setMarkImpactModelByName("book_anchored", 0.5);
  EXPECT_EQ(e.markImpactModel(),
            LiquidationEngine::MarkImpactModel::BookAnchored);
  EXPECT_DOUBLE_EQ(e.markImpactWeight(), 0.5);
  e.setMarkImpactModelByName("book_only");
  EXPECT_EQ(e.markImpactModel(), LiquidationEngine::MarkImpactModel::BookOnly);
  e.setMarkImpactModelByName("none");
  EXPECT_EQ(e.markImpactModel(), LiquidationEngine::MarkImpactModel::None);
  // Unknown name → no-op, prior model + weight retained.
  e.setMarkImpactModelByName("book_anchored", 0.7);
  e.setMarkImpactModelByName("garbage", 0.1);
  EXPECT_EQ(e.markImpactModel(),
            LiquidationEngine::MarkImpactModel::BookAnchored);
  EXPECT_DOUBLE_EQ(e.markImpactWeight(), 0.7);
}

TEST(LiquidationEngine, MaxCascadeDepthZeroDisablesCascading)
{
  // Even with BookOnly model, depth 0 → one pass only.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setExecutor(&ex);
  e.setMarkImpactModel(LiquidationEngine::MarkImpactModel::BookOnly);
  e.setMaxCascadeDepth(0);
  feedBook(ex, BTC, {{80.0, 100.0}}, {{81.0, 100.0}});
  e.openPosition(pos(1, 10.0, 100.0, 10.0));
  const auto out = e.onMark(BTC, 80.0);
  EXPECT_EQ(out.liquidationsCount, 1u);
  EXPECT_EQ(e.cascadeSizesPerTick().size(), 1u);
}

TEST(LiquidationEngine, BookOnlyImpactCascadesSecondOrderLiquidation)
{
  // Stronger setup: position 2 is healthy under the initial tape
  // mark (80) but tips underwater once the book-mid (40.5) becomes
  // the new mark after pass 1. With BookOnly + depth>0, the engine
  // should hit pass 2 and liquidate it within the same onMark call.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setAdlEnabled(false);
  e.setExecutor(&ex);
  e.setMarkImpactModel(LiquidationEngine::MarkImpactModel::BookOnly);
  e.setMaxCascadeDepth(3);

  // Book mid is 40.5 — far below the tape mark of 80.
  feedBook(ex, BTC, {{40.0, 100.0}}, {{41.0, 100.0}});

  // Position 1: deeply underwater at tape mark 80 (entry 100, equity 10).
  e.openPosition(pos(1, 10.0, 100.0, 10.0));
  // Position 2: marginally underwater at 80 (equity 220), but
  // healthy enough that pass 1 selection picks both — to isolate
  // cascade behaviour, give position 2 enough equity to survive at
  // mark=80 and lose at mark=40.5.
  // notional@80 = 800, MM 0.5% → req = 4. equity+upnl = 220-200 = 20 > 4. OK.
  // notional@40.5 = 405, req = 2.025. equity+upnl = 220-595 < 0. Liquidate.
  e.openPosition(pos(2, 10.0, 100.0, 220.0));

  const auto out = e.onMark(BTC, 80.0);
  // Two passes recorded (one per liquidation round).
  EXPECT_EQ(e.cascadeSizesPerTick().size(), 2u);
  EXPECT_EQ(out.liquidationsCount, 2u);
}

TEST(LiquidationEngine, BookAnchoredImpactBlendsMarks)
{
  // BookAnchored weight 1.0 ≡ BookOnly; weight 0.0 ≡ None. Test the
  // 1.0 case to confirm wiring; the parametric blend is exercised
  // implicitly in cascade tests.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setAdlEnabled(false);
  e.setExecutor(&ex);
  e.setMarkImpactModel(LiquidationEngine::MarkImpactModel::BookAnchored, 1.0);
  e.setMaxCascadeDepth(3);

  feedBook(ex, BTC, {{40.0, 100.0}}, {{41.0, 100.0}});
  e.openPosition(pos(1, 10.0, 100.0, 10.0));
  e.openPosition(pos(2, 10.0, 100.0, 220.0));

  e.onMark(BTC, 80.0);
  EXPECT_EQ(e.cascadeSizesPerTick().size(), 2u);
}
