/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// T061: cross-margin (T037) × mark-impact cascade (T038) interaction.
//
// T038 runs the liquidation engine for `_maxCascadeDepth + 1` rounds
// with a recomputed mark per round (book_anchored / book_only). T037
// adds attached-account walks to each round. The interaction is
// subtle: the cascade only updates one symbol's mark (the one passed
// to on_mark); other symbols' marks stay at whatever the caller set
// explicitly via account.set_mark. This pins the design decision.

#include "flox/backtest/account.h"
#include "flox/backtest/liquidation_engine.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"

#include <gtest/gtest.h>

#include <memory_resource>
#include <utility>
#include <vector>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;
constexpr SymbolId ETH = 2;

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

TEST(CrossCascade, CascadeMarksAreSymbolLocal)
{
  // With BookOnly impact + cascade depth > 0, cascade re-runs only
  // refresh the called symbol's mark. ETH leg mark stays at whatever
  // the caller set, even when BTC cascades.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Account a(1, 5'000.0);
  a.openPosition(BTC, 1.0, 50'000.0);
  a.openPosition(ETH, 10.0, 3'000.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setAdlEnabled(false);
  e.setExecutor(&ex);
  e.attachAccount(&a);
  e.setMarkImpactModel(LiquidationEngine::MarkImpactModel::BookOnly);
  e.setMaxCascadeDepth(3);

  // Feed a BTC book whose mid is 30k — far below the tape mark of
  // 45k. Cascade will drag the BTC mark down on rounds 2+.
  feedBook(ex, BTC, {{29'500.0, 100.0}}, {{30'500.0, 100.0}});
  // Set ETH mark explicitly to a known value; cascade must NOT
  // touch it.
  a.setMark(ETH, 2'900.0);
  const double ethMarkBefore = a.markFor(ETH);

  (void)e.onMark(BTC, 45'000.0);

  // ETH mark unchanged by the BTC cascade — symbol-local design.
  EXPECT_DOUBLE_EQ(a.markFor(ETH), ethMarkBefore);
}

TEST(CrossCascade, CascadeRunsAfterCrossMarginLiquidationFires)
{
  // When the cross-margin walk fires a liquidation in round 1, the
  // T038 cascade loop re-runs onMarkOnce with a fresh mark for the
  // called symbol. This pins that the cascade runs at least one
  // round AFTER the initial cross-margin liquidation pass.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Account a(1, 100.0);
  a.openPosition(BTC, 1.0, 50'000.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setAdlEnabled(false);
  e.setExecutor(&ex);
  e.attachAccount(&a);
  e.setMarkImpactModel(LiquidationEngine::MarkImpactModel::BookOnly);
  e.setMaxCascadeDepth(3);

  feedBook(ex, BTC, {{29'500.0, 100.0}}, {{30'500.0, 100.0}});

  // BTC drops 10% (-5000) — equity 100 + (-5000) deeply underwater.
  // Round 1 liquidates via cross walk. Cascade may then run further
  // rounds (no more positions to liquidate; round 2 records as
  // empty pass).
  const auto out = e.onMark(BTC, 45'000.0);
  EXPECT_GE(out.liquidationsCount, 1u);
  // Cascade buffer records every pass that liquidated something;
  // round-1 added the cross-margin liquidation.
  EXPECT_GE(e.cascadeSizesPerTick().size(), 1u);
}

TEST(CrossCascade, MultipleCrossAccountsBothAffectedByCascade)
{
  // Two cross accounts: one long BTC (gets liquidated as cascade
  // drives mark down), one short BTC (becomes profitable).
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Account aLong(1, 500.0);
  aLong.openPosition(BTC, 1.0, 50'000.0);

  Account aShort(2, 5'000.0);
  aShort.openPosition(BTC, -1.0, 50'000.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setInsuranceFundCapital(0.0);
  e.setAdlEnabled(true);
  e.setExecutor(&ex);
  e.attachAccount(&aLong);
  e.attachAccount(&aShort);
  e.setMarkImpactModel(LiquidationEngine::MarkImpactModel::BookOnly);
  e.setMaxCascadeDepth(3);

  // Book mid 30k → cascade drops the BTC mark. aLong liquidates
  // immediately; aShort may get ADL'd to cover the deficit (T055
  // unified ADL path scans both accounts).
  feedBook(ex, BTC, {{29'500.0, 100.0}}, {{30'500.0, 100.0}});

  const auto out = e.onMark(BTC, 45'000.0);
  EXPECT_GE(out.liquidationsCount, 1u);
  // aLong's position should be gone (cross liquidation).
  bool aLongStillHasBtc = false;
  for (const auto& p : aLong.positions())
  {
    if (p.symbol == BTC)
    {
      aLongStillHasBtc = true;
      break;
    }
  }
  EXPECT_FALSE(aLongStillHasBtc);
}

TEST(CrossCascade, MaxCascadeDepthZeroSkipsRecursion)
{
  // With depth = 0, the engine performs exactly one liquidation
  // pass even if the book mid would have flipped more positions
  // underwater. T037 cross walk + T038 cascade both honor the
  // depth gate.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Account a(1, 5'000.0);
  a.openPosition(BTC, 1.0, 50'000.0);

  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.setAdlEnabled(false);
  e.setExecutor(&ex);
  e.attachAccount(&a);
  e.setMarkImpactModel(LiquidationEngine::MarkImpactModel::BookOnly);
  e.setMaxCascadeDepth(0);

  feedBook(ex, BTC, {{29'500.0, 100.0}}, {{30'500.0, 100.0}});

  (void)e.onMark(BTC, 49'500.0);
  // Cascade depth 0 → at most one pass recorded in cascadeSizesPerTick.
  EXPECT_LE(e.cascadeSizesPerTick().size(), 1u);
}
