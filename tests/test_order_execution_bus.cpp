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
  CountingListener(SubscriberId id, std::atomic<int>& c)
      : IOrderExecutionListener(id), counter(c) {}

  void onOrderSubmitted(const Order&) override {}
  void onOrderAccepted(const Order&) override {}
  void onOrderPartiallyFilled(const Order&, Quantity) override {}
  void onOrderFilled(const Order& order) override
  {
    ++counter;
    last = order;
  }
  void onOrderCanceled(const Order&) override {}
  void onOrderExpired(const Order&) override {}
  void onOrderRejected(const Order&, const std::string&) override {}
  void onOrderReplaced(const Order&, const Order&) override {}

  Order last{};
  std::atomic<int>& counter;
};

}  // namespace

TEST(OrderExecutionBusTest, SubscribersReceiveFill)
{
  auto bus = std::make_unique<OrderExecutionBus>();

  std::atomic<int> c1{0}, c2{0};
  auto l1 = std::make_unique<CountingListener>(1, c1);
  auto l2 = std::make_unique<CountingListener>(2, c2);

  bus->subscribe(l1.get());
  bus->subscribe(l2.get());

  bus->start();

  OrderEvent ev{};
  ev.status = OrderEventStatus::FILLED;
  ev.order.symbol = 1;
  ev.order.side = Side::BUY;
  ev.order.quantity = Quantity::fromDouble(1.0);

  const auto seq = bus->publish(ev);
  bus->waitConsumed(seq);

  EXPECT_EQ(c1.load(), 1);
  EXPECT_EQ(c2.load(), 1);

  bus->stop();
}
