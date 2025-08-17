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
#include <chrono>
#include <memory>
#include <thread>

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
  CountingListener(SubscriberId id, std::atomic<int>& c) : IOrderExecutionListener(id), counter(c) {}
  void onOrderSubmitted(const Order&) override {}
  void onOrderAccepted(const Order&) override {}
  void onOrderPartiallyFilled(const Order&, Quantity) override {}
  void onOrderFilled(const Order& o) override
  {
    last = o;
    ++counter;
  }
  void onOrderCanceled(const Order&) override {}
  void onOrderExpired(const Order&) override {}
  void onOrderRejected(const Order&, const std::string&) override {}
  void onOrderReplaced(const Order&, const Order&) override {}

  Order last{};
  std::atomic<int>& counter;
};

class SlowListener : public IOrderExecutionListener
{
 public:
  SlowListener(SubscriberId id, std::atomic<int>& c, std::chrono::milliseconds d)
      : IOrderExecutionListener(id), counter(c), delay(d) {}
  void onOrderSubmitted(const Order&) override {}
  void onOrderAccepted(const Order&) override {}
  void onOrderPartiallyFilled(const Order&, Quantity) override {}
  void onOrderFilled(const Order&) override
  {
    std::this_thread::sleep_for(delay);
    ++counter;
  }
  void onOrderCanceled(const Order&) override {}
  void onOrderExpired(const Order&) override {}
  void onOrderRejected(const Order&, const std::string&) override {}
  void onOrderReplaced(const Order&, const Order&) override {}

  std::atomic<int>& counter;
  std::chrono::milliseconds delay;
};

OrderEvent makeFilled()
{
  OrderEvent ev{};
  ev.status = OrderEventStatus::FILLED;
  ev.order.symbol = 42;
  ev.order.side = Side::BUY;
  ev.order.quantity = Quantity::fromDouble(1.0);
  return ev;
}

}  // namespace

TEST(OrderExecutionBusTest, WaitsForAllRequiredConsumers)
{
  auto bus = std::make_unique<OrderExecutionBus>();

  std::atomic<int> counter{0};
  auto l1 = std::make_unique<CountingListener>(1, counter);
  auto l2 = std::make_unique<CountingListener>(2, counter);

  bus->subscribe(l1.get());
  bus->subscribe(l2.get());

  bus->start();

  auto ev = makeFilled();
  const auto seq = bus->publish(ev);
  bus->waitConsumed(seq);

  EXPECT_EQ(counter.load(), 2);

  bus->stop();
}

TEST(OrderExecutionBusTest, OptionalConsumerDoesNotGate)
{
  auto bus = std::make_unique<OrderExecutionBus>();

  std::atomic<int> reqCount{0};
  std::atomic<int> optCount{0};

  auto fastReq = std::make_unique<CountingListener>(10, reqCount);
  auto slowOpt = std::make_unique<SlowListener>(20, optCount, std::chrono::milliseconds(10));

  bus->subscribe(fastReq.get());
  bus->subscribe(slowOpt.get(), false);

  bus->start();

  const auto seq = bus->publish(makeFilled());
  auto t0 = std::chrono::steady_clock::now();
  bus->waitConsumed(seq);
  auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0);

  EXPECT_EQ(reqCount.load(), 1);
  EXPECT_LT(dt.count(), 5);

  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  EXPECT_EQ(optCount.load(), 1);

  bus->stop();
}

TEST(OrderExecutionBusTest, FlushWaitsAllPublished)
{
  auto bus = std::make_unique<OrderExecutionBus>();

  std::atomic<int> c1{0}, c2{0};
  auto a = std::make_unique<CountingListener>(100, c1);
  auto b = std::make_unique<CountingListener>(200, c2);

  bus->subscribe(a.get());
  bus->subscribe(b.get());
  bus->start();

  constexpr int N = 1000;
  for (int i = 0; i < N; ++i)
  {
    bus->publish(makeFilled());
  }

  bus->flush();

  EXPECT_EQ(c1.load(), N);
  EXPECT_EQ(c2.load(), N);

  bus->stop();
}
