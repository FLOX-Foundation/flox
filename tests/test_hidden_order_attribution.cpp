/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/execution/live_queue_position_estimator.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;
constexpr int64_t SEC = 1'000'000'000LL;
}  // namespace

TEST(HiddenOrderAttribution, IgnoreModePreservesLegacyBehaviour)
{
  LiveQueuePositionEstimator est;
  est.setHiddenOrderPolicy(HiddenOrderPolicy::Ignore);
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0),
                    /*orderId=*/1, Quantity::fromDouble(0.5),
                    /*levelQtyNow=*/Quantity::fromDouble(2.0), 0);

  est.onTradeWithFlag(BTC, Price::fromDouble(50000.0),
                      Quantity::fromDouble(1.0), 1 * SEC, /*isHidden=*/true);

  auto snap = est.snapshot(1);
  ASSERT_TRUE(snap.has_value());
  // Ignore mode treats flagged trade as visible — queue-ahead drops.
  EXPECT_LT(snap->queueAheadEst.toDouble(), 2.0);
  EXPECT_EQ(snap->hiddenVolumeSeen.toDouble(), 0.0);
}

TEST(HiddenOrderAttribution, TrustTradeFlagSkipsHiddenDeduction)
{
  LiveQueuePositionEstimator est;
  est.setHiddenOrderPolicy(HiddenOrderPolicy::TrustTradeFlag);
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0), 1,
                    Quantity::fromDouble(0.5), Quantity::fromDouble(2.0), 0);

  // Hidden-flagged trade: queue-ahead unchanged.
  est.onTradeWithFlag(BTC, Price::fromDouble(50000.0),
                      Quantity::fromDouble(1.0), 1 * SEC, /*isHidden=*/true);

  auto snap = est.snapshot(1);
  ASSERT_TRUE(snap.has_value());
  EXPECT_EQ(snap->queueAheadEst.toDouble(), 2.0);
  EXPECT_EQ(snap->hiddenVolumeSeen.toDouble(), 1.0);
}

TEST(HiddenOrderAttribution, TrustTradeFlagDeductsForVisibleTrade)
{
  LiveQueuePositionEstimator est;
  est.setHiddenOrderPolicy(HiddenOrderPolicy::TrustTradeFlag);
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0), 1,
                    Quantity::fromDouble(0.5), Quantity::fromDouble(2.0), 0);

  est.onTradeWithFlag(BTC, Price::fromDouble(50000.0),
                      Quantity::fromDouble(1.0), 1 * SEC, /*isHidden=*/false);

  auto snap = est.snapshot(1);
  ASSERT_TRUE(snap.has_value());
  EXPECT_LT(snap->queueAheadEst.toDouble(), 2.0);
  EXPECT_EQ(snap->hiddenVolumeSeen.toDouble(), 0.0);
}

TEST(HiddenOrderAttribution, InferModeAttributesExcessTradeToHidden)
{
  LiveQueuePositionEstimator est;
  est.setHiddenOrderPolicy(HiddenOrderPolicy::InferIfTradeExceedsVisible);
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0), 1,
                    Quantity::fromDouble(0.5), Quantity::fromDouble(2.0), 0);

  // Trade reports 5.0 — visible level total is 2.0. Excess 3.0 is
  // attributed to hidden flow; visible 2.0 deducts queue normally.
  est.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(5.0), 1 * SEC);

  auto snap = est.snapshot(1);
  ASSERT_TRUE(snap.has_value());
  EXPECT_EQ(snap->hiddenVolumeSeen.toDouble(), 3.0);
}

TEST(HiddenOrderAttribution, InferModeIgnoresWhenTradeWithinVisible)
{
  LiveQueuePositionEstimator est;
  est.setHiddenOrderPolicy(HiddenOrderPolicy::InferIfTradeExceedsVisible);
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0), 1,
                    Quantity::fromDouble(0.5), Quantity::fromDouble(2.0), 0);

  // Trade 1.0 < visible 2.0 — nothing to infer.
  est.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(1.0), 1 * SEC);

  auto snap = est.snapshot(1);
  ASSERT_TRUE(snap.has_value());
  EXPECT_EQ(snap->hiddenVolumeSeen.toDouble(), 0.0);
}

TEST(HiddenOrderAttribution, ConfidenceUnchangedUnderHiddenAttribution)
{
  LiveQueuePositionEstimator est;
  est.setHiddenOrderPolicy(HiddenOrderPolicy::TrustTradeFlag);
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0), 1,
                    Quantity::fromDouble(0.5), Quantity::fromDouble(2.0), 0);

  // Hidden trade — should NOT trigger proportional-shrink confidence
  // hit (we know what happened).
  est.onTradeWithFlag(BTC, Price::fromDouble(50000.0),
                      Quantity::fromDouble(1.0), 1 * SEC, /*isHidden=*/true);

  auto snap = est.snapshot(1, 1 * SEC);
  ASSERT_TRUE(snap.has_value());
  EXPECT_GT(snap->confidence, 0.95);
}

TEST(HiddenOrderAttribution, AccumulatesHiddenAcrossMultipleTrades)
{
  LiveQueuePositionEstimator est;
  est.setHiddenOrderPolicy(HiddenOrderPolicy::TrustTradeFlag);
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0), 1,
                    Quantity::fromDouble(0.5), Quantity::fromDouble(2.0), 0);

  est.onTradeWithFlag(BTC, Price::fromDouble(50000.0),
                      Quantity::fromDouble(0.5), 1 * SEC, true);
  est.onTradeWithFlag(BTC, Price::fromDouble(50000.0),
                      Quantity::fromDouble(0.3), 2 * SEC, true);
  est.onTradeWithFlag(BTC, Price::fromDouble(50000.0),
                      Quantity::fromDouble(0.2), 3 * SEC, true);

  auto snap = est.snapshot(1, 3 * SEC);
  ASSERT_TRUE(snap.has_value());
  EXPECT_NEAR(snap->hiddenVolumeSeen.toDouble(), 1.0, 1e-9);
}
