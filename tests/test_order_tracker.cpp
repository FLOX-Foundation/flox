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

  const OrderState* state = tracker.get(order.id);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->localOrder.id, 42);
  EXPECT_EQ(state->exchangeOrderId, "abc123");
  EXPECT_EQ(state->status.load(), OrderEventStatus::SUBMITTED);
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

  const OrderState* state = tracker.get(order.id);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->filled.load().toDouble(), 1.0);
  EXPECT_EQ(state->status.load(), OrderEventStatus::FILLED);
}

TEST(OrderTrackerTest, CancelAndReject)
{
  OrderTracker tracker;

  Order order;
  order.id = 2;

  tracker.onSubmitted(order, "ex2");
  tracker.onCanceled(order.id);
  const auto* cancelState = tracker.get(order.id);
  ASSERT_NE(cancelState, nullptr);
  EXPECT_EQ(cancelState->status.load(), OrderEventStatus::CANCELED);

  Order order2;
  order2.id = 3;

  tracker.onSubmitted(order2, "ex3");
  tracker.onRejected(order2.id, "Bad request");
  const auto* rejectState = tracker.get(order2.id);
  ASSERT_NE(rejectState, nullptr);
  EXPECT_EQ(rejectState->status.load(), OrderEventStatus::REJECTED);
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

  const auto* replacedOld = tracker.get(oldOrder.id);
  const auto* replacedNew = tracker.get(newOrder.id);

  ASSERT_NE(replacedOld, nullptr);
  ASSERT_NE(replacedNew, nullptr);
  EXPECT_EQ(replacedOld->status.load(), OrderEventStatus::REPLACED);
  EXPECT_EQ(replacedNew->status.load(), OrderEventStatus::SUBMITTED);
  EXPECT_EQ(replacedNew->exchangeOrderId, "new-id");
}
