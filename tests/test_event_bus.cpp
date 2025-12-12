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
#include <thread>
#include <vector>

#include "flox/util/eventing/event_bus.h"

namespace flox
{

struct TestEvent
{
  using Listener = struct ITestListener
  {
    virtual ~ITestListener() = default;
    virtual void onEvent(const TestEvent& e) = 0;
  };

  int value{0};
  uint64_t tickSequence{0};
};

template <>
struct EventDispatcher<TestEvent>
{
  static void dispatch(const TestEvent& event, TestEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};

}  // namespace flox

namespace
{

using namespace flox;

class CountingListener : public TestEvent::Listener
{
 public:
  void onEvent(const TestEvent& e) override
  {
    ++count;
    lastValue = e.value;
    lastSeq = e.tickSequence;
  }

  std::atomic<int> count{0};
  std::atomic<int> lastValue{0};
  std::atomic<uint64_t> lastSeq{0};
};

class SlowListener : public TestEvent::Listener
{
 public:
  explicit SlowListener(std::chrono::microseconds delay) : _delay(delay) {}

  void onEvent(const TestEvent& e) override
  {
    std::this_thread::sleep_for(_delay);
    ++count;
    lastValue = e.value;
  }

  std::atomic<int> count{0};
  std::atomic<int> lastValue{0};

 private:
  std::chrono::microseconds _delay;
};

using TestBus = EventBus<TestEvent, 64, 8>;
using SmallBus = EventBus<TestEvent, 4, 2>;

// =============================================================================
// Subscribe tests
// =============================================================================

TEST(EventBusTest, SubscribeReturnsTrue)
{
  TestBus bus;
  CountingListener listener;

  EXPECT_TRUE(bus.subscribe(&listener));
  EXPECT_EQ(bus.consumerCount(), 1);
}

TEST(EventBusTest, SubscribeNullReturnsFalse)
{
  TestBus bus;
  EXPECT_FALSE(bus.subscribe(nullptr));
  EXPECT_EQ(bus.consumerCount(), 0);
}

TEST(EventBusTest, SubscribeAfterStartReturnsFalse)
{
  TestBus bus;
  CountingListener listener1;
  CountingListener listener2;

  EXPECT_TRUE(bus.subscribe(&listener1));
  bus.start();

  EXPECT_FALSE(bus.subscribe(&listener2));
  EXPECT_EQ(bus.consumerCount(), 1);

  bus.stop();
}

TEST(EventBusTest, SubscribeExceedsMaxReturnsFalse)
{
  SmallBus bus;  // MaxConsumers = 2
  CountingListener l1, l2, l3;

  EXPECT_TRUE(bus.subscribe(&l1));
  EXPECT_TRUE(bus.subscribe(&l2));
  EXPECT_FALSE(bus.subscribe(&l3));
  EXPECT_EQ(bus.consumerCount(), 2);
}

// =============================================================================
// Basic publish/consume tests
// =============================================================================

TEST(EventBusTest, SingleConsumerReceivesEvents)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);
  bus.start();

  for (int i = 0; i < 10; ++i)
  {
    TestEvent e{.value = i};
    bus.publish(e);
  }

  bus.flush();
  bus.stop();

  EXPECT_EQ(listener.count.load(), 10);
  EXPECT_EQ(listener.lastValue.load(), 9);
}

TEST(EventBusTest, MultipleConsumersReceiveAllEvents)
{
  TestBus bus;
  CountingListener l1, l2, l3;

  bus.subscribe(&l1);
  bus.subscribe(&l2);
  bus.subscribe(&l3);
  bus.start();

  constexpr int kEventCount = 50;
  for (int i = 0; i < kEventCount; ++i)
  {
    bus.publish(TestEvent{.value = i});
  }

  bus.flush();
  bus.stop();

  EXPECT_EQ(l1.count.load(), kEventCount);
  EXPECT_EQ(l2.count.load(), kEventCount);
  EXPECT_EQ(l3.count.load(), kEventCount);
}

TEST(EventBusTest, TickSequenceIsSet)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);
  bus.start();

  bus.publish(TestEvent{.value = 42});
  bus.flush();
  bus.stop();

  EXPECT_EQ(listener.lastSeq.load(), 0);  // First event has seq 0
}

TEST(EventBusTest, SequenceIncrementsCorrectly)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);
  bus.start();

  for (int i = 0; i < 5; ++i)
  {
    auto seq = bus.publish(TestEvent{.value = i});
    EXPECT_EQ(seq, i);
  }

  bus.flush();
  bus.stop();

  EXPECT_EQ(listener.lastSeq.load(), 4);
}

// =============================================================================
// tryPublish with timeout tests
// =============================================================================

TEST(EventBusTest, TryPublishSucceedsWithoutBackpressure)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);
  bus.start();

  auto [result, seq] = bus.tryPublish(TestEvent{.value = 123}, std::chrono::microseconds{1000});

  EXPECT_EQ(result, TestBus::PublishResult::SUCCESS);
  EXPECT_EQ(seq, 0);

  bus.flush();
  bus.stop();

  EXPECT_EQ(listener.count.load(), 1);
  EXPECT_EQ(listener.lastValue.load(), 123);
}

TEST(EventBusTest, TryPublishTimeoutOnBackpressure)
{
  SmallBus smallBus;  // Very small buffer (4 slots, 2 consumers max)
  SlowListener slowListener(std::chrono::milliseconds{100});

  smallBus.subscribe(&slowListener);
  smallBus.start();

  // Fill the buffer
  for (int i = 0; i < 4; ++i)
  {
    smallBus.publish(TestEvent{.value = i});
  }

  // This should timeout because buffer is full and consumer is slow
  auto [result, seq] = smallBus.tryPublish(TestEvent{.value = 999}, std::chrono::microseconds{100});

  // May succeed or timeout depending on timing
  EXPECT_TRUE(result == SmallBus::PublishResult::SUCCESS || result == SmallBus::PublishResult::TIMEOUT);

  smallBus.stop();
}

TEST(EventBusTest, PublishReturnsNegativeWhenStopped)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);
  // Don't start the bus

  auto seq = bus.publish(TestEvent{.value = 1});
  EXPECT_EQ(seq, -1);
}

TEST(EventBusTest, TryPublishReturnsStoppedWhenNotRunning)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);
  // Don't start

  auto [result, seq] = bus.tryPublish(TestEvent{.value = 1}, std::chrono::microseconds{1000});

  EXPECT_EQ(result, TestBus::PublishResult::STOPPED);
  EXPECT_EQ(seq, -1);
}

// =============================================================================
// Start/Stop tests
// =============================================================================

TEST(EventBusTest, DoubleStartIsIdempotent)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);
  bus.start();
  bus.start();  // Second start should be no-op

  bus.publish(TestEvent{.value = 1});
  bus.flush();
  bus.stop();

  EXPECT_EQ(listener.count.load(), 1);
}

TEST(EventBusTest, DoubleStopIsIdempotent)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);
  bus.start();

  bus.publish(TestEvent{.value = 1});
  bus.flush();

  bus.stop();
  bus.stop();  // Second stop should be no-op

  EXPECT_EQ(listener.count.load(), 1);
}

TEST(EventBusTest, DrainOnStopProcessesRemainingEvents)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);
  bus.enableDrainOnStop();
  bus.start();

  for (int i = 0; i < 10; ++i)
  {
    bus.publish(TestEvent{.value = i});
  }

  // Stop without explicit flush
  bus.stop();

  EXPECT_EQ(listener.count.load(), 10);
}

// =============================================================================
// Wrap-around tests
// =============================================================================

TEST(EventBusTest, BufferWrapAroundWorks)
{
  EventBus<TestEvent, 8, 2> smallBus;  // Capacity 8
  CountingListener listener;

  smallBus.subscribe(&listener);
  smallBus.start();

  // Publish more than buffer capacity
  constexpr int kCount = 100;
  for (int i = 0; i < kCount; ++i)
  {
    smallBus.publish(TestEvent{.value = i});
  }

  smallBus.flush();
  smallBus.stop();

  EXPECT_EQ(listener.count.load(), kCount);
  EXPECT_EQ(listener.lastValue.load(), kCount - 1);
}

// =============================================================================
// Concurrent publish tests
// =============================================================================

TEST(EventBusTest, ConcurrentPublishersWork)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);
  bus.start();

  constexpr int kThreads = 4;
  constexpr int kEventsPerThread = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t)
  {
    threads.emplace_back([&bus, t]
                         {
      for (int i = 0; i < kEventsPerThread; ++i)
      {
        bus.publish(TestEvent{.value = t * 1000 + i});
      } });
  }

  for (auto& t : threads)
  {
    t.join();
  }

  bus.flush();
  bus.stop();

  EXPECT_EQ(listener.count.load(), kThreads * kEventsPerThread);
}

// =============================================================================
// Required vs optional consumer tests
// =============================================================================

TEST(EventBusTest, OptionalConsumerDoesNotBlockGating)
{
  TestBus bus;
  CountingListener requiredListener;
  SlowListener optionalSlowListener(std::chrono::milliseconds{50});

  bus.subscribe(&requiredListener, true);       // required
  bus.subscribe(&optionalSlowListener, false);  // optional

  bus.start();

  for (int i = 0; i < 10; ++i)
  {
    bus.publish(TestEvent{.value = i});
  }

  bus.waitConsumed(9);  // Wait for required consumer only

  bus.stop();

  EXPECT_EQ(requiredListener.count.load(), 10);
  // Optional listener may not have finished all events
  EXPECT_LE(optionalSlowListener.count.load(), 10);
}

// =============================================================================
// Regression test for optional consumer race condition during wrap-around
// This test catches the bug where required consumer's reclaim could destroy
// events before optional consumer had a chance to process them.
// =============================================================================

TEST(EventBusTest, OptionalConsumerReceivesAllEventsOnWrapAround)
{
  // Small buffer to force wrap-around quickly
  EventBus<TestEvent, 8, 4> smallBus;

  CountingListener fastRequired;
  // Slow optional consumer that takes 100µs per event
  SlowListener slowOptional(std::chrono::microseconds{100});

  smallBus.subscribe(&fastRequired, true);   // required - will be fast
  smallBus.subscribe(&slowOptional, false);  // optional - will be slow

  smallBus.start();

  // Publish more events than buffer capacity to trigger wrap-around
  // The fast required consumer will process quickly and could trigger reclaim
  // before the slow optional consumer processes the events
  constexpr int kEventCount = 32;  // 4x buffer capacity
  for (int i = 0; i < kEventCount; ++i)
  {
    smallBus.publish(TestEvent{.value = i});
  }

  // Wait for all consumers to finish
  smallBus.flush();

  // Give slow consumer extra time to finish (in case flush() only waits for required)
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (slowOptional.count.load() < kEventCount && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }

  smallBus.stop();

  EXPECT_EQ(fastRequired.count.load(), kEventCount);
  // This is the critical assertion - optional consumer MUST receive ALL events
  // even during wrap-around when required consumer finishes first
  EXPECT_EQ(slowOptional.count.load(), kEventCount)
      << "Optional consumer missed events during wrap-around. "
         "This indicates a race condition between required consumer's reclaim "
         "and optional consumer's event processing.";
}

// Test that optional consumer doesn't process timeout placeholders
TEST(EventBusTest, OptionalConsumerSkipsTimeoutPlaceholders)
{
  SmallBus smallBus;  // 4 slots
  SlowListener slowConsumer(std::chrono::milliseconds{50});

  smallBus.subscribe(&slowConsumer, false);  // optional
  smallBus.start();

  // Fill buffer
  for (int i = 0; i < 4; ++i)
  {
    smallBus.publish(TestEvent{.value = i});
  }

  // Try to publish with very short timeout - should create timeout placeholder
  auto [result, seq] = smallBus.tryPublish(TestEvent{.value = 999}, std::chrono::microseconds{1});

  // Wait for consumer to process
  smallBus.flush();

  // Give extra time for slow consumer
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (slowConsumer.count.load() < 4 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }

  smallBus.stop();

  // Should have processed initial 4 events
  EXPECT_GE(slowConsumer.count.load(), 4);

  // If timeout occurred, the placeholder should be skipped (not crash, not count as event)
  if (result == SmallBus::PublishResult::TIMEOUT)
  {
    // Exactly 4 events should be processed (timeout placeholder skipped)
    EXPECT_EQ(slowConsumer.count.load(), 4);
  }
  else
  {
    // If publish succeeded, 5 events should be processed
    EXPECT_EQ(slowConsumer.count.load(), 5);
  }
}

}  // namespace
