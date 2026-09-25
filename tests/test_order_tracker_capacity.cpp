/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// the execution tracker fix, core finding 6 (second half).
//
// config::ORDER_TRACKER_CAPACITY exists, carries a FLOX_DEFAULT_ORDER_TRACKER_CAPACITY
// override hook, and is read by nothing. OrderTracker is default-constructed
// everywhere and its unordered_map has no bound at all. The tracker has to
// take the capacity, expose it, and honour it.
//
// needs: explicit OrderTracker(size_t capacity);
// needs: OrderTracker() with the default capacity config::ORDER_TRACKER_CAPACITY;
// needs: size_t OrderTracker::capacity() const noexcept;

#include "flox/engine/engine_config.h"
#include "flox/execution/order_tracker.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{

Order makeOrder(OrderId id)
{
  Order order;
  order.id = id;
  order.symbol = 7;
  order.side = Side::BUY;
  order.price = Price::fromDouble(100.0);
  order.quantity = Quantity::fromDouble(1.0);
  return order;
}

}  // namespace

TEST(OrderTrackerCapacityTest, DefaultCapacityComesFromEngineConfig)
{
  OrderTracker tracker;
  EXPECT_EQ(tracker.capacity(), static_cast<size_t>(config::ORDER_TRACKER_CAPACITY));
}

TEST(OrderTrackerCapacityTest, ExplicitCapacityIsReported)
{
  OrderTracker tracker{8};
  EXPECT_EQ(tracker.capacity(), 8u);
}

// A tracker built with capacity 8 holds at most 8 entries, both while the
// churn runs and after an explicit prune.
TEST(OrderTrackerCapacityTest, CapacityEightHoldsAtMostEight)
{
  OrderTracker tracker{8};

  for (size_t i = 0; i < 100; ++i)
  {
    const OrderId id = static_cast<OrderId>(i) + 1;
    ASSERT_TRUE(tracker.onSubmitted(makeOrder(id), "ex"));
    ASSERT_TRUE(tracker.onCanceled(id));
    ASSERT_LE(tracker.totalOrderCount(), 8u) << "after " << (i + 1) << " cycles";
  }

  tracker.pruneTerminal();

  EXPECT_LE(tracker.totalOrderCount(), 8u);
  EXPECT_EQ(tracker.activeOrderCount(), 0u);
}

TEST(OrderTrackerCapacityTest, ReplaceChainHoldsAtMostCapacity)
{
  OrderTracker tracker{8};

  OrderId current = 1;
  ASSERT_TRUE(tracker.onSubmitted(makeOrder(current), "ex-1"));
  for (size_t i = 0; i < 100; ++i)
  {
    const OrderId next = current + 1;
    ASSERT_TRUE(tracker.onReplaced(current, makeOrder(next), "ex"));
    current = next;
    ASSERT_LE(tracker.totalOrderCount(), 8u) << "after " << (i + 1) << " amends";
  }

  EXPECT_EQ(tracker.activeOrderCount(), 1u);
  ASSERT_TRUE(tracker.exists(current));
  EXPECT_EQ(tracker.getStatus(current), OrderEventStatus::SUBMITTED);
}

// Trimming to capacity must sacrifice terminal history, never a live order:
// with capacity 8 and 8 open orders the tracker keeps all 8, and after four of
// them go terminal and four fresh ones arrive, every live order is still there.
TEST(OrderTrackerCapacityTest, LiveOrdersAreNotEvictedToMakeRoom)
{
  OrderTracker tracker{8};

  for (OrderId id = 1; id <= 8; ++id)
  {
    ASSERT_TRUE(tracker.onSubmitted(makeOrder(id), "ex"));
  }

  EXPECT_EQ(tracker.totalOrderCount(), 8u);
  EXPECT_EQ(tracker.activeOrderCount(), 8u);

  for (OrderId id = 1; id <= 4; ++id)
  {
    ASSERT_TRUE(tracker.onCanceled(id));
  }

  const std::vector<OrderId> live = {5, 6, 7, 8, 9, 10, 11, 12};
  for (OrderId id = 9; id <= 12; ++id)
  {
    ASSERT_TRUE(tracker.onSubmitted(makeOrder(id), "ex"));
  }

  EXPECT_LE(tracker.totalOrderCount(), 8u);
  EXPECT_EQ(tracker.activeOrderCount(), 8u);
  for (OrderId id : live)
  {
    EXPECT_TRUE(tracker.isActive(id)) << "live order " << id << " was evicted";
  }

  // Saturated with live orders and no history left to reclaim. Making room by
  // evicting a live entry would leave a working order on the venue that the
  // process no longer tracks, so the insert is refused instead.
  EXPECT_FALSE(tracker.onSubmitted(makeOrder(13), "ex"));
  EXPECT_FALSE(tracker.exists(13));
  EXPECT_EQ(tracker.totalOrderCount(), 8u);
  EXPECT_EQ(tracker.activeOrderCount(), 8u);
  for (OrderId id : live)
  {
    EXPECT_TRUE(tracker.isActive(id)) << "live order " << id << " was evicted for a refused submit";
  }
}
