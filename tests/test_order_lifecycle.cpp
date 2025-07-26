/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/execution/events/order_event.h"

#include <gtest/gtest.h>

using namespace flox;

TEST(OrderLifecycleTest, Defaults)
{
  OrderEvent orderEvent{};
  EXPECT_EQ(orderEvent.type, OrderEventType::INVALID);
  EXPECT_TRUE(orderEvent.order.filledQuantity.isZero());
  EXPECT_EQ(orderEvent.order.createdAt, TimePoint{});
  EXPECT_FALSE(orderEvent.order.exchangeTimestamp.has_value());
  EXPECT_FALSE(orderEvent.order.lastUpdated.has_value());
  EXPECT_FALSE(orderEvent.order.expiresAfter.has_value());
}
