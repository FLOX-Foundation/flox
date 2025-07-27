/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <memory>

#include "flox/engine/abstract_subscriber.h"
#include "flox/execution/bus/order_execution_bus.h"
#include "flox/execution/events/order_event.h"
#include "flox/execution/order.h"

using namespace flox;

namespace
{

class CountingListener : public IOrderExecutionListener
{
 public:
  explicit CountingListener(SubscriberId id, std::atomic<int>& c)
      : IOrderExecutionListener(id), _counter(c) {}

  void onOrderSubmitted(const Order&) override {}
  void onOrderAccepted(const Order&) override {}
  void onOrderPartiallyFilled(const Order&, Quantity) override {}
  void onOrderFilled(const Order& order) override
  {
    ++_counter;
    last = order;
  }
  void onOrderCanceled(const Order&) override {}
  void onOrderExpired(const Order&) override {}
  void onOrderRejected(const Order&, const std::string&) override {}
  void onOrderReplaced(const Order&, const Order&) override {}

  Order last{};

 private:
  std::atomic<int>& _counter;
};

}  // namespace

TEST(OrderExecutionBusTest, SubscribersReceiveFill)
{
  OrderExecutionBus bus;
  bus.enableDrainOnStop();
  std::atomic<int> c1{0}, c2{0};
  auto l1 = std::make_shared<CountingListener>(1, c1);
  auto l2 = std::make_shared<CountingListener>(2, c2);
  bus.subscribe(l1);
  bus.subscribe(l2);

  bus.start();

  OrderEvent event{};
  event.status = OrderEventStatus::FILLED;
  event.order.symbol = 1;
  event.order.side = Side::BUY;
  event.order.quantity = Quantity::fromDouble(1.0);

  bus.publish(event);
  bus.stop();

  EXPECT_EQ(c1.load(), 1);
  EXPECT_EQ(c2.load(), 1);
}
