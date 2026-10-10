/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/execution/events/order_event.h"

#include <gtest/gtest.h>

using namespace lrvx;

TEST(OrderLifecycleTest, Defaults)
{
  OrderEvent orderEvent{};
  EXPECT_EQ(orderEvent.status, OrderEventStatus::NEW);
  EXPECT_TRUE(orderEvent.order.filledQuantity.isZero());
  EXPECT_EQ(orderEvent.order.createdAt, TimePoint{});
  EXPECT_FALSE(orderEvent.order.exchangeTimestamp.has_value());
  EXPECT_FALSE(orderEvent.order.lastUpdated.has_value());
  EXPECT_FALSE(orderEvent.order.expiresAfter.has_value());
}
