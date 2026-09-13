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
#include <condition_variable>
#include <mutex>
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

// An event whose listener batches work -- it only commits what it was handed
// when the bus tells it the ingress is drained. Exactly the shape of a
// listener that amortises an fsync over a batch.
struct BatchedEvent
{
  using Listener = struct IBatchedListener
  {
    virtual ~IBatchedListener() = default;
    virtual void onEvent(const BatchedEvent& e) = 0;
    virtual void onBatchEnd() = 0;
  };

  int value{0};
  uint64_t tickSequence{0};
};

template <>
struct EventDispatcher<BatchedEvent>
{
  static void dispatch(const BatchedEvent& event, BatchedEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
  static void endOfBatch(BatchedEvent::Listener& listener) { listener.onBatchEnd(); }
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

// Parks the consumer inside the handler for the very first event it sees, so
// backpressure is a fact rather than a race: nothing is reclaimed until the
// test says so.
class GatedListener : public TestEvent::Listener
{
 public:
  void onEvent(const TestEvent& e) override
  {
    if (!_entered.exchange(true))
    {
      std::unique_lock lk(_m);
      _cv.wait(lk, [&]
               { return _open; });
    }
    ++count;
    lastValue = e.value;
  }

  bool waitEntered(std::chrono::milliseconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!_entered.load() && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return _entered.load();
  }

  void release()
  {
    {
      std::lock_guard lk(_m);
      _open = true;
    }
    _cv.notify_all();
  }

  std::atomic<int> count{0};
  std::atomic<int> lastValue{0};

 private:
  std::atomic<bool> _entered{false};
  std::mutex _m;
  std::condition_variable _cv;
  bool _open{false};
};

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (pred())
    {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return pred();
}

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

// A timeout under backpressure is a refusal to publish, and a refusal must
// cost the events already in the ring nothing. The old assertion here was
// "SUCCESS or TIMEOUT", which covers every value tryPublish can return while
// the bus runs, so it could not fail -- and it stood on exactly the scenario
// where the timeout path used to overwrite a slot the consumer had not read.
// What is worth asserting is the delivery count once the backpressure clears.
TEST(EventBusTest, TryPublishTimeoutOnBackpressure)
{
  SmallBus smallBus;  // Very small buffer (4 slots, 2 consumers max)
  GatedListener gated;

  smallBus.subscribe(&gated);
  smallBus.start();

  // Fill the ring. The consumer parks inside the handler for the first event,
  // so nothing is reclaimed and the next publish must hit backpressure.
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_GE(smallBus.publish(TestEvent{.value = i}), 0);
  }
  ASSERT_TRUE(gated.waitEntered(std::chrono::seconds{2}));

  auto [r1, s1] = smallBus.tryPublish(TestEvent{.value = 901}, std::chrono::microseconds{2000});
  auto [r2, s2] = smallBus.tryPublish(TestEvent{.value = 902}, std::chrono::microseconds{2000});

  EXPECT_EQ(r1, SmallBus::PublishResult::TIMEOUT);
  EXPECT_EQ(r2, SmallBus::PublishResult::TIMEOUT);
  EXPECT_EQ(s1, -1);
  EXPECT_EQ(s2, -1);

  gated.release();

  EXPECT_TRUE(waitFor([&]
                      { return gated.count.load() >= 4; }, std::chrono::seconds{2}))
      << "delivered " << gated.count.load() << " of the 4 events that publish() accepted; "
                                               "a refused tryPublish must not consume events already in the ring";

  smallBus.stop();

  EXPECT_EQ(gated.count.load(), 4);
  EXPECT_EQ(smallBus.stats().dropped, 2u);
}

// The same scenario, carried one step further -- the bus has to keep
// working. Two refused publishes used to stamp slots that a lagging consumer
// had not read yet, leaving it spinning on a sequence that no longer existed.
TEST(EventBusTest, TryPublishTimeoutKeepsTheBusUsable)
{
  SmallBus smallBus;
  GatedListener gated;

  smallBus.subscribe(&gated);
  smallBus.start();

  for (int i = 0; i < 4; ++i)
  {
    EXPECT_GE(smallBus.publish(TestEvent{.value = i}), 0);
  }
  ASSERT_TRUE(gated.waitEntered(std::chrono::seconds{2}));

  EXPECT_EQ(smallBus.tryPublish(TestEvent{.value = 901}, std::chrono::microseconds{2000}).first,
            SmallBus::PublishResult::TIMEOUT);
  EXPECT_EQ(smallBus.tryPublish(TestEvent{.value = 902}, std::chrono::microseconds{2000}).first,
            SmallBus::PublishResult::TIMEOUT);

  gated.release();
  ASSERT_TRUE(waitFor([&]
                      { return gated.count.load() >= 4; }, std::chrono::seconds{2}))
      << "bus wedged after the refused publishes: delivered " << gated.count.load() << " of 4";

  // A refused publish takes no sequence, so the ring is exactly where the last
  // accepted publish left it and delivery continues.
  const int64_t seq = smallBus.publish(TestEvent{.value = 903});
  EXPECT_EQ(seq, 4) << "a refused tryPublish must not burn a sequence number";

  EXPECT_TRUE(waitFor([&]
                      { return gated.count.load() >= 5; }, std::chrono::seconds{2}))
      << "the bus stopped delivering after a refused publish";
  EXPECT_EQ(gated.lastValue.load(), 903);

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

// stop() is half of the ISubsystem contract and nothing in it says the bus is
// single-use. A restarted bus used to accept publishes, hand back valid
// sequence numbers and deliver nothing at all: the consumer threads restart
// from sequence 0 while the ring counters carry the previous run's positions.
TEST(EventBusTest, StopThenStartDeliversAgain)
{
  TestBus bus;
  CountingListener listener;

  bus.subscribe(&listener);

  bus.start();
  for (int i = 0; i < 5; ++i)
  {
    EXPECT_GE(bus.publish(TestEvent{.value = i}), 0);
  }
  EXPECT_TRUE(waitFor([&]
                      { return listener.count.load() >= 5; }, std::chrono::seconds{2}));
  bus.stop();
  ASSERT_EQ(listener.count.load(), 5) << "first run already broken";

  bus.start();
  for (int i = 0; i < 5; ++i)
  {
    EXPECT_GE(bus.publish(TestEvent{.value = 100 + i}), 0);
  }
  EXPECT_TRUE(waitFor([&]
                      { return listener.count.load() >= 10; }, std::chrono::seconds{2}))
      << "after stop()/start() the bus delivered " << (listener.count.load() - 5)
      << " of 5 events while publish() kept returning valid sequence numbers";
  bus.stop();

  EXPECT_EQ(listener.count.load(), 10);
  EXPECT_EQ(listener.lastValue.load(), 104);
}

// A restarted bus must also stay publishable: with the ring gating left at the
// previous run's positions the producer wedges a capacity's worth of events in.
TEST(EventBusTest, StopThenStartKeepsThePublisherRunning)
{
  SmallBus bus;  // 4 slots, so a stale gate bites after 4 publishes
  CountingListener listener;

  bus.subscribe(&listener);

  bus.start();
  for (int i = 0; i < 8; ++i)
  {
    EXPECT_GE(bus.publish(TestEvent{.value = i}), 0);
  }
  EXPECT_TRUE(waitFor([&]
                      { return listener.count.load() >= 8; }, std::chrono::seconds{2}));
  bus.stop();

  bus.start();
  int accepted = 0;
  for (int i = 0; i < 16; ++i)
  {
    const auto [r, seq] = bus.tryPublish(TestEvent{.value = 200 + i},
                                         std::chrono::microseconds{200000});
    if (r == SmallBus::PublishResult::SUCCESS)
    {
      ++accepted;
    }
  }
  EXPECT_EQ(accepted, 16) << "the producer wedged on a restarted bus";
  EXPECT_TRUE(waitFor([&]
                      { return listener.count.load() >= 24; }, std::chrono::seconds{3}))
      << "delivered " << listener.count.load() << " of 24";
  bus.stop();
}

// The drain that runs on stop() delivers events, and for a listener that
// batches its work that is only half the contract: without the end-of-batch
// edge the listener holds everything it was handed and never commits it. The
// run cap is what makes this deterministic -- the main loop stops after
// kMaxConsumeRun events, and everything past that leaves through the drain.
TEST(EventBusTest, DrainOnStopEndsTheBatch)
{
  using BatchBus = EventBus<BatchedEvent, 4096, 2>;

  struct Listener : BatchedEvent::Listener
  {
    void onEvent(const BatchedEvent& e) override
    {
      if (e.value == 0)
      {
        std::unique_lock lk(m);
        entered.store(true);
        cv.wait(lk, [&]
                { return open; });
      }
      ++dispatched;
    }
    void onBatchEnd() override
    {
      committed.store(dispatched.load());
      ++batchEnds;
    }
    void release()
    {
      {
        std::lock_guard lk(m);
        open = true;
      }
      cv.notify_all();
    }

    std::atomic<int> dispatched{0};
    std::atomic<int> committed{0};
    std::atomic<int> batchEnds{0};
    std::atomic<bool> entered{false};
    std::mutex m;
    std::condition_variable cv;
    bool open{false};
  };

  constexpr int kEvents = 2000;  // > the 1024-event run cap of the main loop

  BatchBus bus;
  Listener listener;
  bus.subscribe(&listener);
  bus.enableDrainOnStop();
  bus.start();

  for (int i = 0; i < kEvents; ++i)
  {
    ASSERT_GE(bus.publish(BatchedEvent{.value = i}), 0);
  }
  ASSERT_TRUE(waitFor([&]
                      { return listener.entered.load(); }, std::chrono::seconds{2}));

  std::thread stopper([&]
                      { bus.stop(); });
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  listener.release();
  stopper.join();

  EXPECT_EQ(listener.dispatched.load(), kEvents);
  EXPECT_EQ(listener.committed.load(), listener.dispatched.load())
      << "the drain handed the listener " << listener.dispatched.load()
      << " events but only " << listener.committed.load()
      << " of them were ever closed out by an end-of-batch";
  EXPECT_GE(listener.batchEnds.load(), 2);
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

}  // namespace
