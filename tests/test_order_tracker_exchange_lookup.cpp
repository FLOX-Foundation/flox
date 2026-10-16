/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>
#include "lrvx/execution/order_tracker.h"

using namespace lrvx;

namespace
{

Order makeOrder(OrderId id)
{
  Order order;
  order.id = id;
  order.quantity = Quantity::fromDouble(1.0);
  return order;
}

}  // namespace

TEST(OrderTrackerExchangeLookup, FindsAfterSubmitAndMissesUnknown)
{
  OrderTracker tracker;

  tracker.onSubmitted(makeOrder(42), "hl-oid-777");

  auto hit = tracker.findLocalIdByExchangeOrderId("hl-oid-777");
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(*hit, 42u);

  EXPECT_FALSE(tracker.findLocalIdByExchangeOrderId("hl-oid-778").has_value());
}

TEST(OrderTrackerExchangeLookup, EmptyIdIsNeverIndexed)
{
  OrderTracker tracker;

  // A pre-ack or paper submit carries no exchange id. It must not become a
  // "" key that every other id-less order would then collide with.
  tracker.onSubmitted(makeOrder(1), "");

  EXPECT_FALSE(tracker.findLocalIdByExchangeOrderId("").has_value());
}

TEST(OrderTrackerExchangeLookup, TerminalEntryResolvesUntilPruned)
{
  OrderTracker tracker;

  tracker.onSubmitted(makeOrder(7), "x-100");
  tracker.onCanceled(7);

  // A late report keyed by the venue id must still find the order while the
  // tracker remembers it...
  auto hit = tracker.findLocalIdByExchangeOrderId("x-100");
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(*hit, 7u);

  // ...and stop resolving exactly when the entry itself is gone.
  tracker.pruneTerminal();
  EXPECT_FALSE(tracker.findLocalIdByExchangeOrderId("x-100").has_value());
  EXPECT_FALSE(tracker.get(7).has_value());
}

TEST(OrderTrackerExchangeLookup, ReplaceIndexesNewIdAndKeepsOldUntilPrune)
{
  OrderTracker tracker;

  tracker.onSubmitted(makeOrder(1), "old-id");
  tracker.onReplaced(1, makeOrder(2), "new-id");

  // Late fills on the superseded order arrive keyed by the old id.
  auto oldHit = tracker.findLocalIdByExchangeOrderId("old-id");
  ASSERT_TRUE(oldHit.has_value());
  EXPECT_EQ(*oldHit, 1u);

  auto newHit = tracker.findLocalIdByExchangeOrderId("new-id");
  ASSERT_TRUE(newHit.has_value());
  EXPECT_EQ(*newHit, 2u);

  tracker.pruneTerminal();
  EXPECT_FALSE(tracker.findLocalIdByExchangeOrderId("old-id").has_value());
  auto survivor = tracker.findLocalIdByExchangeOrderId("new-id");
  ASSERT_TRUE(survivor.has_value());
  EXPECT_EQ(*survivor, 2u);
}

TEST(OrderTrackerExchangeLookup, ReusedExchangeIdPointsAtNewestHolder)
{
  OrderTracker tracker;

  tracker.onSubmitted(makeOrder(1), "shared");
  tracker.onCanceled(1);
  tracker.onSubmitted(makeOrder(2), "shared");

  auto hit = tracker.findLocalIdByExchangeOrderId("shared");
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(*hit, 2u);

  // Pruning the old holder must not take the new mapping with it.
  tracker.pruneTerminal();
  auto stillThere = tracker.findLocalIdByExchangeOrderId("shared");
  ASSERT_TRUE(stillThere.has_value());
  EXPECT_EQ(*stillThere, 2u);
}

TEST(OrderTrackerExchangeLookup, CapacityEvictionDropsIndexEntriesWithTheirOrders)
{
  OrderTracker tracker(2);

  tracker.onSubmitted(makeOrder(1), "a");
  tracker.onCanceled(1);
  tracker.onSubmitted(makeOrder(2), "b");
  tracker.onCanceled(2);

  // Full of terminal history; this insert recycles it via makeRoomLocked().
  ASSERT_TRUE(tracker.onSubmitted(makeOrder(3), "c"));
  EXPECT_EQ(tracker.totalOrderCount(), 1u);

  EXPECT_FALSE(tracker.findLocalIdByExchangeOrderId("a").has_value());
  EXPECT_FALSE(tracker.findLocalIdByExchangeOrderId("b").has_value());
  auto hit = tracker.findLocalIdByExchangeOrderId("c");
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(*hit, 3u);
}
