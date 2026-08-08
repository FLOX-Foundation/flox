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
#include <stdexcept>
#include <thread>
#include <vector>

#include "flox/util/eventing/event_bus.h"

namespace flox
{

struct HealthTestEvent
{
  using Listener = struct IHealthTestListener
  {
    virtual ~IHealthTestListener() = default;
    virtual void onEvent(const HealthTestEvent& e) = 0;
  };

  int value{0};
  uint64_t tickSequence{0};
};

template <>
struct EventDispatcher<HealthTestEvent>
{
  static void dispatch(const HealthTestEvent& event, HealthTestEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};

}  // namespace flox

namespace
{

using namespace flox;

constexpr size_t kCapacity = 64;
using SmallBus = EventBus<HealthTestEvent, kCapacity, 4>;

class Counting : public HealthTestEvent::Listener
{
 public:
  void onEvent(const HealthTestEvent& e) override
  {
    ++count;
    lastValue = e.value;
  }
  std::atomic<int> count{0};
  std::atomic<int> lastValue{0};
};

class Blocking : public HealthTestEvent::Listener
{
 public:
  void onEvent(const HealthTestEvent&) override
  {
    ++entered;
    while (blocked.load())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ++count;
  }
  std::atomic<int> entered{0};
  std::atomic<bool> blocked{true};
  std::atomic<int> count{0};
};

class ThrowingOnFirst : public HealthTestEvent::Listener
{
 public:
  void onEvent(const HealthTestEvent&) override
  {
    throw std::runtime_error("handler failure");
  }
};

struct CallbackRecord
{
  std::atomic<int> stalledEvents{0};
  std::atomic<int> deadEvents{0};
  std::atomic<int> healthyEvents{0};
};

void recordCallback(uint32_t, SmallBus::ConsumerHealth state, void* user)
{
  auto* rec = static_cast<CallbackRecord*>(user);
  switch (state)
  {
    case SmallBus::ConsumerHealth::STALLED:
      ++rec->stalledEvents;
      break;
    case SmallBus::ConsumerHealth::DEAD:
      ++rec->deadEvents;
      break;
    case SmallBus::ConsumerHealth::HEALTHY:
      ++rec->healthyEvents;
      break;
  }
}

class SlowEach : public HealthTestEvent::Listener
{
 public:
  void onEvent(const HealthTestEvent&) override
  {
    // Slow but always returns to the loop top -- drop-behind fires there.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++count;
  }
  std::atomic<int> count{0};
};

TEST(EventBusHealth, DropBehindOptionalDoesNotStallPublisher)
{
  SmallBus bus;
  SmallBus::HealthConfig cfg;
  cfg.dropBehindOptional = true;
  cfg.dropBehindSlack = kCapacity / 2;
  bus.setHealthConfig(cfg);

  Counting fast;
  SlowEach slow;
  bus.subscribe(&fast, /*required=*/true);
  bus.subscribe(&slow, /*required=*/false);
  bus.start();

  // Flood far beyond capacity while the optional consumer is stuck in its
  // first event. Without drop-behind the publisher would freeze at the
  // reclaim fence once the ring wraps.
  constexpr int kEvents = int(kCapacity) * 8;
  for (int i = 0; i < kEvents; ++i)
  {
    HealthTestEvent ev;
    ev.value = i;
    ASSERT_GE(bus.publish(ev), 0) << "publisher stalled at event " << i;
  }

  // Fast required consumer sees everything.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (fast.count.load() < kEvents && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(fast.count.load(), kEvents);

  bus.stop();

  const auto stats = bus.stats();
  EXPECT_GT(stats.droppedBehind, 0u);
  EXPECT_EQ(stats.published, uint64_t(kEvents));
}

TEST(EventBusHealth, CheckHealthReportsStallAndRecovery)
{
  SmallBus bus;
  CallbackRecord rec;
  SmallBus::HealthConfig cfg;
  cfg.stallThreshold = std::chrono::milliseconds(20);
  cfg.callback = &recordCallback;
  cfg.callbackUser = &rec;
  bus.setHealthConfig(cfg);

  Blocking blocking;
  bus.subscribe(&blocking, /*required=*/true);
  bus.start();

  HealthTestEvent ev;
  ev.value = 1;
  bus.publish(ev);
  ev.value = 2;
  bus.publish(ev);  // pending work behind the blocked consumer

  // Wait until the consumer is inside the handler, then let the stall age.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (blocking.entered.load() == 0 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_GT(blocking.entered.load(), 0);

  bus.checkHealth();  // baseline sweep records lastSeen
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  const auto sweep = bus.checkHealth();
  EXPECT_EQ(sweep.stalled, 1u);
  EXPECT_EQ(rec.stalledEvents.load(), 1);

  // Recovery: unblock, wait for progress, next sweep reports healthy.
  blocking.blocked = false;
  while (blocking.count.load() < 2 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto sweep2 = bus.checkHealth();
  EXPECT_EQ(sweep2.stalled, 0u);
  EXPECT_EQ(rec.healthyEvents.load(), 1);

  bus.stop();
}

TEST(EventBusHealth, ThrowingHandlerIsDetectedAsDead)
{
  SmallBus bus;
  CallbackRecord rec;
  SmallBus::HealthConfig cfg;
  cfg.callback = &recordCallback;
  cfg.callbackUser = &rec;
  bus.setHealthConfig(cfg);

  ThrowingOnFirst throwing;
  Counting healthy;
  bus.subscribe(&throwing, /*required=*/false);
  bus.subscribe(&healthy, /*required=*/true);
  bus.start();

  HealthTestEvent ev;
  ev.value = 7;
  bus.publish(ev);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  SmallBus::HealthSweep sweep{};
  while (std::chrono::steady_clock::now() < deadline)
  {
    sweep = bus.checkHealth();
    if (sweep.dead > 0)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(sweep.dead, 1u);
  EXPECT_EQ(rec.deadEvents.load(), 1);

  // The rest of the bus keeps working.
  ev.value = 8;
  bus.publish(ev);
  while (healthy.count.load() < 2 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(healthy.count.load(), 2);

  bus.stop();
}

TEST(EventBusHealth, DeadRequiredConsumerStopsBusUnderStopPolicy)
{
  SmallBus bus;
  SmallBus::HealthConfig cfg;
  cfg.deadPolicy = SmallBus::DeadConsumerPolicy::STOP_BUS;
  bus.setHealthConfig(cfg);

  ThrowingOnFirst throwing;
  bus.subscribe(&throwing, /*required=*/true);
  bus.start();

  HealthTestEvent ev;
  ev.value = 1;
  bus.publish(ev);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool stopped = false;
  while (std::chrono::steady_clock::now() < deadline)
  {
    bus.checkHealth();
    HealthTestEvent probe;
    probe.value = 2;
    if (bus.publish(probe) < 0)
    {
      stopped = true;  // publish on a stopped bus returns -1
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(stopped);
}

TEST(EventBusHealth, MonitorThreadSweepsAutomatically)
{
  SmallBus bus;
  CallbackRecord rec;
  SmallBus::HealthConfig cfg;
  cfg.stallThreshold = std::chrono::milliseconds(10);
  cfg.enableMonitorThread = true;
  cfg.callback = &recordCallback;
  cfg.callbackUser = &rec;
  bus.setHealthConfig(cfg);

  Blocking blocking;
  bus.subscribe(&blocking, /*required=*/true);
  bus.start();

  HealthTestEvent ev;
  ev.value = 1;
  bus.publish(ev);
  ev.value = 2;
  bus.publish(ev);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (rec.stalledEvents.load() == 0 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_GT(rec.stalledEvents.load(), 0);

  blocking.blocked = false;
  bus.stop();
}

}  // namespace
