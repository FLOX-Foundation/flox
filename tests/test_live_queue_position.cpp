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

TEST(LiveQueuePositionEstimator, PlacementCapturesArrivalQueue)
{
  LiveQueuePositionEstimator est;
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0),
                    /*orderId=*/1, Quantity::fromDouble(0.5),
                    /*levelQtyNow=*/Quantity::fromDouble(2.0),
                    /*tsNs=*/0);

  auto snap = est.snapshot(1);
  ASSERT_TRUE(snap.has_value());
  EXPECT_EQ(snap->orderId, 1u);
  // We arrived behind 2.0 units of resting volume.
  EXPECT_EQ(snap->queueAheadEst.toDouble(), 2.0);
  EXPECT_GT(snap->confidence, 0.99);
}

TEST(LiveQueuePositionEstimator, TradeDeductsQueueAhead)
{
  LiveQueuePositionEstimator est;
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0),
                    1, Quantity::fromDouble(0.5),
                    Quantity::fromDouble(2.0), 0);

  // 1.0 unit trades through the level — half of the queue ahead
  // of us should be gone.
  est.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(1.0), 1 * SEC);

  auto snap = est.snapshot(1);
  ASSERT_TRUE(snap.has_value());
  EXPECT_EQ(snap->queueAheadEst.toDouble(), 1.0);
  // Trade-attributed deductions don't degrade confidence on their
  // own (modulo time decay).
  EXPECT_GT(snap->confidence, 0.95);
}

TEST(LiveQueuePositionEstimator, ProportionalShrinkDropsConfidence)
{
  LiveQueuePositionEstimator est;
  est.setShrinkAttributionFactor(0.5);
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0),
                    1, Quantity::fromDouble(0.5),
                    Quantity::fromDouble(2.0), 0);

  // Level shrinks from 2.0 -> 1.0 without a trade explaining it ⇒
  // cancellation attribution. Confidence drops by the configured
  // shrink factor.
  est.onLevelUpdate(BTC, Side::BUY, Price::fromDouble(50000.0),
                    Quantity::fromDouble(1.0), 1 * SEC);

  auto snap = est.snapshot(1);
  ASSERT_TRUE(snap.has_value());
  // 0.5 (default decay over 1s of 60s half-life is negligible).
  EXPECT_NEAR(snap->confidence, 0.5, 0.02);
  // Queue-ahead also drops via the proportional-shrink heuristic.
  EXPECT_LT(snap->queueAheadEst.toDouble(), 2.0);
}

TEST(LiveQueuePositionEstimator, TimeDecayReducesConfidence)
{
  LiveQueuePositionEstimator est;
  est.setConfidenceHalfLifeNs(SEC);  // 1-second half-life

  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0),
                    1, Quantity::fromDouble(0.5),
                    Quantity::fromDouble(2.0), 0);

  // Query 2 seconds later — confidence should be ~0.25 (two half-lives).
  auto snap = est.snapshot(1, /*nowNs=*/2 * SEC);
  ASSERT_TRUE(snap.has_value());
  EXPECT_NEAR(snap->confidence, 0.25, 0.02);
}

TEST(LiveQueuePositionEstimator, OrderRemovedOnCancel)
{
  LiveQueuePositionEstimator est;
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0),
                    1, Quantity::fromDouble(0.5),
                    Quantity::fromDouble(2.0), 0);
  EXPECT_EQ(est.trackedOrderCount(), 1u);

  est.onOrderCancelled(1, 1 * SEC);
  EXPECT_EQ(est.trackedOrderCount(), 0u);
  EXPECT_FALSE(est.snapshot(1).has_value());
}

TEST(LiveQueuePositionEstimator, OrderRemovedOnFullFill)
{
  LiveQueuePositionEstimator est;
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0),
                    1, Quantity::fromDouble(0.5),
                    Quantity::fromDouble(2.0), 0);

  // Queue ahead drains and our order fully fills. The tracker's
  // onTrade applies fills to waiting orders; calling onOrderFilled
  // with the cumulative fill triggers removal once total is met.
  est.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(2.5), 1 * SEC);
  est.onOrderFilled(1, Quantity::fromDouble(0.5), 1 * SEC);

  EXPECT_FALSE(est.snapshot(1).has_value());
}

TEST(LiveQueuePositionEstimator, MultipleOrdersTrackedIndependently)
{
  LiveQueuePositionEstimator est;
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0),
                    1, Quantity::fromDouble(0.5),
                    Quantity::fromDouble(2.0), 0);
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(49999.0),
                    2, Quantity::fromDouble(0.3),
                    Quantity::fromDouble(1.5), 0);

  EXPECT_EQ(est.trackedOrderCount(), 2u);

  // Trade hits only the better-priced level.
  est.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(0.5), 1 * SEC);

  auto a = est.snapshot(1);
  auto b = est.snapshot(2);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_LT(a->queueAheadEst.toDouble(), 2.0);
  EXPECT_EQ(b->queueAheadEst.toDouble(), 1.5);  // untouched
}

TEST(LiveQueuePositionEstimator, ConfidenceClampedToUnitInterval)
{
  LiveQueuePositionEstimator est;
  est.onOrderPlaced(BTC, Side::BUY, Price::fromDouble(50000.0),
                    1, Quantity::fromDouble(0.5),
                    Quantity::fromDouble(2.0), 0);
  // Many shrinks should not push confidence negative.
  for (int i = 0; i < 100; ++i)
  {
    est.onLevelUpdate(BTC, Side::BUY, Price::fromDouble(50000.0),
                      Quantity::fromDouble(0.0), 1 * SEC);
    est.onLevelUpdate(BTC, Side::BUY, Price::fromDouble(50000.0),
                      Quantity::fromDouble(2.0), 1 * SEC);
  }
  auto snap = est.snapshot(1, 0);
  if (snap.has_value())
  {
    EXPECT_GE(snap->confidence, 0.0);
    EXPECT_LE(snap->confidence, 1.0);
  }
}
// W15-T020 — gated test below
