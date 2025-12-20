/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>
#include "flox/execution/order_tracker.h"

using namespace flox;

TEST(OrderTrackerTest, SubmitAndGet)
{
  OrderTracker tracker;

  Order order;
  order.id = 42;
  order.symbol = 101;
  order.price = Price::fromDouble(123.45);
  order.quantity = Quantity::fromDouble(0.5);

  tracker.onSubmitted(order, "abc123");

  auto state = tracker.get(order.id);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->localOrder.id, 42);
  EXPECT_EQ(state->exchangeOrderId, "abc123");
  EXPECT_EQ(state->status, OrderEventStatus::SUBMITTED);
}

TEST(OrderTrackerTest, FillUpdatesQuantity)
{
  OrderTracker tracker;

  Order order;
  order.id = 1;
  order.quantity = Quantity::fromDouble(1.0);

  tracker.onSubmitted(order, "xid");
  tracker.onFilled(order.id, Quantity::fromDouble(0.4));
  tracker.onFilled(order.id, Quantity::fromDouble(0.6));

  auto state = tracker.get(order.id);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->filled.toDouble(), 1.0);
  EXPECT_EQ(state->status, OrderEventStatus::FILLED);
}

TEST(OrderTrackerTest, CancelAndReject)
{
  OrderTracker tracker;

  Order order;
  order.id = 2;

  tracker.onSubmitted(order, "ex2");
  tracker.onCanceled(order.id);
  auto cancelState = tracker.get(order.id);
  ASSERT_TRUE(cancelState.has_value());
  EXPECT_EQ(cancelState->status, OrderEventStatus::CANCELED);

  Order order2;
  order2.id = 3;

  tracker.onSubmitted(order2, "ex3");
  tracker.onRejected(order2.id, "Bad request");
  auto rejectState = tracker.get(order2.id);
  ASSERT_TRUE(rejectState.has_value());
  EXPECT_EQ(rejectState->status, OrderEventStatus::REJECTED);
}

TEST(OrderTrackerTest, ReplaceOrder)
{
  OrderTracker tracker;

  Order oldOrder;
  oldOrder.id = 5;
  oldOrder.quantity = Quantity::fromDouble(1.0);

  Order newOrder;
  newOrder.id = 6;
  newOrder.quantity = Quantity::fromDouble(2.0);

  tracker.onSubmitted(oldOrder, "old-id");
  tracker.onReplaced(oldOrder.id, newOrder, "new-id");

  auto replacedOld = tracker.get(oldOrder.id);
  auto replacedNew = tracker.get(newOrder.id);

  ASSERT_TRUE(replacedOld.has_value());
  ASSERT_TRUE(replacedNew.has_value());
  EXPECT_EQ(replacedOld->status, OrderEventStatus::REPLACED);
  EXPECT_EQ(replacedNew->status, OrderEventStatus::SUBMITTED);
  EXPECT_EQ(replacedNew->exchangeOrderId, "new-id");
}

TEST(OrderTrackerTest, DoubleCancelSafe)
{
  OrderTracker tracker;

  Order order;
  order.id = 10;
  tracker.onSubmitted(order, "ex");

  EXPECT_TRUE(tracker.onCanceled(order.id));
  EXPECT_FALSE(tracker.onCanceled(order.id));
}

TEST(OrderTrackerTest, DuplicateOrderIdRejected)
{
  OrderTracker tracker;

  Order order;
  order.id = 20;
  EXPECT_TRUE(tracker.onSubmitted(order, "ex1"));
  EXPECT_FALSE(tracker.onSubmitted(order, "ex2"));
}

TEST(OrderTrackerTest, PruneTerminal)
{
  OrderTracker tracker;

  Order o1, o2, o3;
  o1.id = 100;
  o2.id = 101;
  o3.id = 102;

  tracker.onSubmitted(o1, "e1");
  tracker.onSubmitted(o2, "e2");
  tracker.onSubmitted(o3, "e3");

  tracker.onCanceled(o1.id);
  tracker.onFilled(o2.id, o2.quantity);

  EXPECT_EQ(tracker.totalOrderCount(), 3);
  EXPECT_EQ(tracker.activeOrderCount(), 1);

  tracker.pruneTerminal();

  EXPECT_EQ(tracker.totalOrderCount(), 1);
  EXPECT_TRUE(tracker.exists(o3.id));
  EXPECT_FALSE(tracker.exists(o1.id));
  EXPECT_FALSE(tracker.exists(o2.id));
}

TEST(OrderTrackerTest, StatusHelpers)
{
  OrderTracker tracker;

  Order order;
  order.id = 30;
  tracker.onSubmitted(order, "ex");

  EXPECT_TRUE(tracker.exists(order.id));
  EXPECT_TRUE(tracker.isActive(order.id));
  EXPECT_EQ(tracker.getStatus(order.id), OrderEventStatus::SUBMITTED);

  tracker.onCanceled(order.id);

  EXPECT_TRUE(tracker.exists(order.id));
  EXPECT_FALSE(tracker.isActive(order.id));
  EXPECT_EQ(tracker.getStatus(order.id), OrderEventStatus::CANCELED);
}
