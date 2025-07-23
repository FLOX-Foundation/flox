/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/execution/order.h"

#include <gtest/gtest.h>
#include <chrono>

using namespace flox;

TEST(OrderLifecycleTest, Defaults)
{
  Order order{};
  EXPECT_EQ(order.status, OrderStatus::NEW);
  EXPECT_TRUE(order.filledQuantity.isZero());
  EXPECT_EQ(order.createdAt, TimePoint{});
  EXPECT_FALSE(order.exchangeTimestamp.has_value());
  EXPECT_FALSE(order.lastUpdated.has_value());
  EXPECT_FALSE(order.expiresAfter.has_value());
}
