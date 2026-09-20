/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Consuming and waiting used to be one thing: a consumer WAS a thread, and the
// only way to make it stop burning a core was to stop the bus. These tests
// cover the seam that separates them -- one step over one consumer, taken by
// whoever wants to take it -- and the two invariants that only become
// interesting once a single thread may step many consumers: one listener's
// failure must not stop the others, and a consumer with no thread of its own
// must still be visible as stalled.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "flox/util/eventing/event_bus.h"

namespace flox
{

struct StepTestEvent
{
  using Listener = struct IStepTestListener
  {
    virtual ~IStepTestListener() = default;
    virtual void onEvent(const StepTestEvent& e) = 0;
  };

  int value{0};
  uint64_t tickSequence{0};
};

template <>
struct EventDispatcher<StepTestEvent>
{
  static void dispatch(const StepTestEvent& event, StepTestEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};

}  // namespace flox

namespace
{

using namespace flox;

constexpr size_t kCapacity = 64;
using StepBus = EventBus<StepTestEvent, kCapacity, 4>;

class Counting : public StepTestEvent::Listener
{
 public:
  void onEvent(const StepTestEvent& e) override
  {
    ++count;
    last = e.value;
  }
  std::atomic<int> count{0};
  std::atomic<int> last{-1};
};

// Throws on the value it was given, delivers everything else. A handler that
// throws on every event cannot show that the slot keeps its progress.
class ThrowsOn : public StepTestEvent::Listener
{
 public:
  explicit ThrowsOn(int bad) : bad_(bad) {}
  void onEvent(const StepTestEvent& e) override
  {
    ++attempts;
    if (e.value == bad_)
    {
      throw std::runtime_error("handler failure");
    }
    ++count;
  }
  std::atomic<int> count{0};
  std::atomic<int> attempts{0};  // deliveries, including the one that threw

 private:
  int bad_;
};

StepTestEvent ev(int v)
{
  StepTestEvent e;
  e.value = v;
  return e;
}

// ---------------------------------------------------------------------------
// The step itself.

TEST(EventBusStep, PollConsumerDeliversWithoutAConsumerThread)
{
  StepBus bus;
  Counting c;
  bus.setOwnConsumerThreads(false);
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true));
  bus.start();

  // Nothing published: the step reports "nothing to do", which is the signal
  // a driver waits on.
  EXPECT_FALSE(bus.pollConsumer(0));
  EXPECT_EQ(c.count.load(), 0);

  for (int i = 0; i < 5; ++i)
  {
    bus.publish(ev(i));
  }

  // One step takes the whole contiguous run, not one event.
  EXPECT_TRUE(bus.pollConsumer(0));
  EXPECT_EQ(c.count.load(), 5);
  EXPECT_EQ(c.last.load(), 4);

  EXPECT_FALSE(bus.pollConsumer(0));
  bus.stop();
}

TEST(EventBusStep, StepPublishesProgressSoThePublisherCanWrap)
{
  StepBus bus;
  Counting c;
  bus.setOwnConsumerThreads(false);
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true));
  bus.start();

  // More events than the ring holds: without a consumer making progress the
  // publisher blocks at the wrap. Stepping is what lets it through.
  std::atomic<bool> done{false};
  std::thread producer(
      [&]
      {
        for (int i = 0; i < static_cast<int>(kCapacity) * 3; ++i)
        {
          bus.publish(ev(i));
        }
        done.store(true);
      });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done.load() && std::chrono::steady_clock::now() < deadline)
  {
    bus.pollConsumer(0);
  }
  const bool gotThrough = done.load();
  if (!gotThrough)
  {
    // The producer is parked at the wrap and would park forever; stopping the
    // bus releases it. A test that fails must still end, and a joinable
    // thread left behind ends the process instead of the test.
    bus.stop();
  }
  producer.join();
  ASSERT_TRUE(gotThrough) << "producer never got through the wrap";

  while (bus.pollConsumer(0))
  {
  }
  EXPECT_EQ(c.count.load(), static_cast<int>(kCapacity) * 3);
  bus.stop();
}

TEST(EventBusStep, OneDriverStepsManyConsumers)
{
  StepBus bus;
  Counting a;
  Counting b;
  bus.setOwnConsumerThreads(false);
  ASSERT_TRUE(bus.subscribe(&a, /*required=*/true));
  ASSERT_TRUE(bus.subscribe(&b, /*required=*/true));
  bus.start();
  ASSERT_EQ(bus.consumerCount(), 2u);

  bus.publish(ev(7));
  for (uint32_t i = 0; i < bus.consumerCount(); ++i)
  {
    bus.pollConsumer(i);
  }
  EXPECT_EQ(a.count.load(), 1);
  EXPECT_EQ(b.count.load(), 1);
  EXPECT_EQ(a.last.load(), 7);
  EXPECT_EQ(b.last.load(), 7);
  bus.stop();
}

TEST(EventBusStep, DrainOnStopStillHappensWithoutConsumerThreads)
{
  StepBus bus;
  Counting c;
  bus.setOwnConsumerThreads(false);
  bus.enableDrainOnStop();
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true));
  bus.start();

  for (int i = 0; i < 4; ++i)
  {
    bus.publish(ev(i));
  }
  ASSERT_EQ(c.count.load(), 0);

  // Nobody stepped the consumer, and nobody will: the events are owed to the
  // listener anyway, exactly as they would be with a thread on its way out.
  bus.stop();
  EXPECT_EQ(c.count.load(), 4);
}

// ---------------------------------------------------------------------------
// A failed listener takes down its slot, not its driver.

TEST(EventBusStep, ThrowingListenerDoesNotStopTheOtherConsumers)
{
  StepBus bus;
  ThrowsOn bad(2);
  Counting good;
  bus.setOwnConsumerThreads(false);
  ASSERT_TRUE(bus.subscribe(&bad, /*required=*/false));
  ASSERT_TRUE(bus.subscribe(&good, /*required=*/true));
  bus.start();

  for (int i = 0; i < 4; ++i)
  {
    bus.publish(ev(i));
  }
  for (uint32_t i = 0; i < bus.consumerCount(); ++i)
  {
    bus.pollConsumer(i);
  }

  // The bad one stopped where it threw and is out of service; the good one
  // got everything. Before the split, a throw returned out of the loop that
  // was driving it -- which under one thread per consumer cost one consumer,
  // and under a shared driver would cost every consumer on that thread.
  EXPECT_TRUE(bus.consumerFailed(0));
  EXPECT_EQ(bad.count.load(), 2);  // values 0 and 1, then the throw on 2
  EXPECT_FALSE(bus.consumerFailed(1));
  EXPECT_EQ(good.count.load(), 4);

  // Further steps on the failed slot do nothing at all, rather than
  // re-entering a handler that is known to throw: the listener is not called
  // again, not even to throw again.
  const int attemptsBefore = bad.attempts.load();
  EXPECT_FALSE(bus.pollConsumer(0));
  bus.publish(ev(5));
  EXPECT_FALSE(bus.pollConsumer(0));
  EXPECT_EQ(bad.attempts.load(), attemptsBefore);

  // And the driver keeps driving the rest: the good consumer took the four
  // it started with plus everything published since.
  bus.publish(ev(9));
  EXPECT_TRUE(bus.pollConsumer(1));
  EXPECT_EQ(good.count.load(), 6);
  bus.stop();
}

TEST(EventBusStep, FailedSlotKeepsTheProgressItHadMade)
{
  StepBus bus;
  ThrowsOn bad(1);
  bus.setOwnConsumerThreads(false);
  ASSERT_TRUE(bus.subscribe(&bad, /*required=*/false));
  bus.start();

  bus.publish(ev(0));
  bus.publish(ev(1));
  bus.publish(ev(2));
  bus.pollConsumer(0);

  ASSERT_TRUE(bus.consumerFailed(0));
  // Progress stops at the last event the listener actually took, not at the
  // one that threw and not at the end of the run: the publisher needs the
  // truth to reclaim slots.
  EXPECT_EQ(bus.stats().consumed, 1u);
  bus.stop();
}

// ---------------------------------------------------------------------------
// Liveness is progress, not presence.

TEST(EventBusStep, ConsumerNobodyStepsReadsAsStalledNotDead)
{
  StepBus bus;
  Counting c;
  StepBus::HealthConfig cfg;
  cfg.stallThreshold = std::chrono::milliseconds(20);
  bus.setHealthConfig(cfg);
  bus.setOwnConsumerThreads(false);
  ASSERT_TRUE(bus.subscribe(&c, /*required=*/true));
  bus.start();

  bus.publish(ev(1));

  // Health used to be "is the consumer's loop running": a consumer with no
  // loop of its own read as DEAD, and a driver that silently stopped stepping
  // one of its consumers read as perfectly healthy. Both are backwards.
  bus.checkHealth();
  EXPECT_EQ(bus.consumerHealth(0), StepBus::ConsumerHealth::HEALTHY);

  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  bus.checkHealth();
  EXPECT_EQ(bus.consumerHealth(0), StepBus::ConsumerHealth::STALLED);

  // Stepping it clears the stall on the next sweep.
  EXPECT_TRUE(bus.pollConsumer(0));
  bus.checkHealth();
  EXPECT_EQ(bus.consumerHealth(0), StepBus::ConsumerHealth::HEALTHY);
  bus.stop();
}

TEST(EventBusStep, DeadMeansTheHandlerThrew)
{
  StepBus bus;
  ThrowsOn bad(0);
  StepBus::HealthConfig cfg;
  cfg.stallThreshold = std::chrono::milliseconds(20);
  bus.setHealthConfig(cfg);
  bus.setOwnConsumerThreads(false);
  ASSERT_TRUE(bus.subscribe(&bad, /*required=*/false));
  bus.start();

  bus.publish(ev(0));
  bus.pollConsumer(0);
  bus.checkHealth();
  EXPECT_EQ(bus.consumerHealth(0), StepBus::ConsumerHealth::DEAD);
  bus.stop();
}

}  // namespace
