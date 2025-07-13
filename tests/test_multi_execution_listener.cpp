/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>
#include "flox/common.h"
#include "flox/engine/abstract_subscriber.h"
#include "flox/execution/abstract_execution_listener.h"
#include "flox/execution/multi_execution_listener.h"

using namespace flox;

class MockExecutionListener : public IOrderExecutionListener
{
 public:
  int acceptedCount = 0;
  int partialCount = 0;
  int filledCount = 0;
  int canceledCount = 0;
  int expiredCount = 0;
  int rejectedCount = 0;
  int replacedCount = 0;
  Order lastOrder;
  Order replacedOld;
  Order replacedNew;

  MockExecutionListener(SubscriberId id) : IOrderExecutionListener(id) {}

  void onOrderSubmitted(const Order& order) override
  {
  }

  void onOrderAccepted(const Order& order) override
  {
    ++acceptedCount;
    lastOrder = order;
  }

  void onOrderPartiallyFilled(const Order& order, Quantity) override
  {
    ++partialCount;
    lastOrder = order;
  }

  void onOrderFilled(const Order& order) override
  {
    ++filledCount;
    lastOrder = order;
  }

  void onOrderCanceled(const Order& order) override
  {
    ++canceledCount;
    lastOrder = order;
  }

  void onOrderExpired(const Order& order) override
  {
    ++expiredCount;
    lastOrder = order;
  }

  void onOrderRejected(const Order& order, const std::string&) override
  {
    ++rejectedCount;
    lastOrder = order;
  }

  void onOrderReplaced(const Order& oldOrder, const Order& newOrder) override
  {
    ++replacedCount;
    replacedOld = oldOrder;
    replacedNew = newOrder;
  }
};

TEST(MultiExecutionListenerTest, CallsAllListeners)
{
  MultiExecutionListener multi(1);
  MockExecutionListener l1(2), l2(3);

  multi.addListener(&l1);
  multi.addListener(&l2);

  Order order{};
  order.symbol = 1;
  order.price = Price::fromDouble(100.0);
  order.quantity = Quantity::fromDouble(1.0);
  multi.onOrderFilled(order);

  EXPECT_EQ(l1.filledCount, 1);
  EXPECT_EQ(l2.filledCount, 1);
  EXPECT_EQ(l1.lastOrder.price, Price::fromDouble(100.0));
  EXPECT_EQ(l2.lastOrder.quantity, Quantity::fromDouble(1.0));
}

TEST(MultiExecutionListenerTest, ForwardsLifecycleCallbacks)
{
  MultiExecutionListener multi(1);
  MockExecutionListener listener(2);

  multi.addListener(&listener);

  Order order{};
  order.symbol = 1;
  order.price = Price::fromDouble(10.0);
  order.quantity = Quantity::fromDouble(1.0);

  multi.onOrderAccepted(order);
  EXPECT_EQ(listener.acceptedCount, 1);

  multi.onOrderPartiallyFilled(order, Quantity::fromDouble(0.5));
  EXPECT_EQ(listener.partialCount, 1);

  multi.onOrderCanceled(order);
  EXPECT_EQ(listener.canceledCount, 1);

  multi.onOrderExpired(order);
  EXPECT_EQ(listener.expiredCount, 1);

  multi.onOrderRejected(order, "");
  EXPECT_EQ(listener.rejectedCount, 1);

  Order newOrder{};
  newOrder.symbol = 2;
  newOrder.price = Price::fromDouble(11.0);
  newOrder.quantity = Quantity::fromDouble(1.0);
  multi.onOrderReplaced(order, newOrder);
  EXPECT_EQ(listener.replacedCount, 1);
  EXPECT_EQ(listener.replacedOld.symbol, 1u);
  EXPECT_EQ(listener.replacedNew.symbol, 2u);
}

TEST(MultiExecutionListenerTest, PreventsDuplicateListeners)
{
  MultiExecutionListener multi(100);
  MockExecutionListener l1(101);

  multi.addListener(&l1);
  multi.addListener(&l1);  // Duplicate

  Order order{};
  order.symbol = 1;
  order.price = Price::fromDouble(100.0);
  order.quantity = Quantity::fromDouble(1.0);
  multi.onOrderFilled(order);

  EXPECT_EQ(l1.filledCount, 1);  // Should only be called once
}
