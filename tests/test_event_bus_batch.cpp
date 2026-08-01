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
#include <numeric>
#include <thread>
#include <vector>

#include "flox/util/eventing/event_bus.h"

namespace flox
{

struct BatchTestEvent
{
  using Listener = struct IBatchTestListener
  {
    virtual ~IBatchTestListener() = default;
    virtual void onEvent(const BatchTestEvent& e) = 0;
  };

  int value{0};
  uint64_t tickSequence{0};
};

template <>
struct EventDispatcher<BatchTestEvent>
{
  template <typename Sub>
  static void dispatch(const BatchTestEvent& event, Sub& listener)
  {
    listener.onEvent(event);
  }
};

}  // namespace flox

namespace
{

using namespace flox;

using Bus = EventBus<BatchTestEvent, 64, 8>;

class RecordingListener : public BatchTestEvent::Listener
{
 public:
  void onEvent(const BatchTestEvent& e) override
  {
    values.push_back(e.value);
    seqs.push_back(e.tickSequence);
    count.store(values.size(), std::memory_order_release);
  }

  std::vector<int> values;
  std::vector<uint64_t> seqs;
  std::atomic<size_t> count{0};
};

// flush() only waits for required consumers; an optional consumer that has
// not caught up loses the tail when stop() lands. Tests that assert on an
// optional consumer have to wait for it themselves.
bool waitForCount(const std::atomic<size_t>& count, size_t expected)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (count.load(std::memory_order_acquire) < expected)
  {
    if (std::chrono::steady_clock::now() > deadline)
    {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

// No inheritance: the static-subscription surface only needs the handler.
struct PlainRecorder
{
  void onEvent(const BatchTestEvent& e)
  {
    values.push_back(e.value);
    seqs.push_back(e.tickSequence);
  }

  std::vector<int> values;
  std::vector<uint64_t> seqs;
};

std::vector<BatchTestEvent> makeEvents(int first, int count)
{
  std::vector<BatchTestEvent> evs(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i)
  {
    evs[static_cast<size_t>(i)].value = first + i;
  }
  return evs;
}

TEST(EventBusBatch, PublishBatchDeliversAllInOrder)
{
  Bus bus;
  RecordingListener a;
  RecordingListener b;
  ASSERT_TRUE(bus.subscribe(&a, true));
  ASSERT_TRUE(bus.subscribe(&b, true));
  bus.start();

  const auto evs = makeEvents(0, 10);
  const int64_t lastSeq = bus.publishBatch(evs.data(), evs.size());
  EXPECT_EQ(lastSeq, 9);
  bus.flush();
  bus.stop();

  ASSERT_EQ(a.values.size(), 10u);
  ASSERT_EQ(b.values.size(), 10u);
  for (int i = 0; i < 10; ++i)
  {
    EXPECT_EQ(a.values[static_cast<size_t>(i)], i);
    EXPECT_EQ(b.values[static_cast<size_t>(i)], i);
  }
}

TEST(EventBusBatch, TickSequenceIsContinuousAcrossMixedPublishing)
{
  Bus bus;
  RecordingListener a;
  ASSERT_TRUE(bus.subscribe(&a, true));
  bus.start();

  BatchTestEvent single;
  single.value = 100;
  bus.publish(single);

  const auto batch = makeEvents(101, 5);
  bus.publishBatch(batch.data(), batch.size());

  single.value = 106;
  bus.publish(single);

  bus.flush();
  bus.stop();

  ASSERT_EQ(a.values.size(), 7u);
  for (size_t i = 0; i < a.values.size(); ++i)
  {
    EXPECT_EQ(a.values[i], 100 + static_cast<int>(i));
    EXPECT_EQ(a.seqs[i], i);
  }
}

TEST(EventBusBatch, WrapsRingManyTimes)
{
  Bus bus;  // capacity 64
  RecordingListener a;
  ASSERT_TRUE(bus.subscribe(&a, true));
  bus.start();

  constexpr int kTotal = 10'000;
  constexpr int kChunk = 24;  // deliberately not a divisor of capacity
  int published = 0;
  while (published < kTotal)
  {
    const int n = std::min(kChunk, kTotal - published);
    const auto evs = makeEvents(published, n);
    ASSERT_GE(bus.publishBatch(evs.data(), static_cast<size_t>(n)), 0);
    published += n;
  }
  bus.flush();
  bus.stop();

  ASSERT_EQ(a.values.size(), static_cast<size_t>(kTotal));
  for (int i = 0; i < kTotal; ++i)
  {
    ASSERT_EQ(a.values[static_cast<size_t>(i)], i);
  }
}

TEST(EventBusBatch, OptionalConsumerReceivesWithoutGating)
{
  Bus bus;
  RecordingListener required;
  RecordingListener optional;
  ASSERT_TRUE(bus.subscribe(&required, true));
  ASSERT_TRUE(bus.subscribe(&optional, false));
  bus.start();

  const auto evs = makeEvents(0, 200);  // forces wraps against gating
  for (size_t off = 0; off < evs.size(); off += 25)
  {
    bus.publishBatch(evs.data() + off, 25);  // capacity 64: batches must stay <= 32
  }
  bus.flush();
  EXPECT_TRUE(waitForCount(optional.count, 200));
  bus.stop();

  EXPECT_EQ(required.values.size(), 200u);
  EXPECT_EQ(optional.values.size(), 200u);
}

TEST(EventBusBatch, StatsCountBatchedTraffic)
{
  Bus bus;
  RecordingListener a;
  RecordingListener b;
  ASSERT_TRUE(bus.subscribe(&a, true));
  ASSERT_TRUE(bus.subscribe(&b, true));
  bus.start();

  const auto evs = makeEvents(0, 30);
  bus.publishBatch(evs.data(), evs.size());
  bus.flush();
  bus.stop();

  const auto stats = bus.stats();
  EXPECT_EQ(stats.published, 30u);
  EXPECT_EQ(stats.consumed, 60u);  // two consumers
  EXPECT_EQ(stats.dropped, 0u);
}

TEST(EventBusBatch, PublishBatchAfterStopReturnsMinusOne)
{
  Bus bus;
  RecordingListener a;
  ASSERT_TRUE(bus.subscribe(&a, true));
  const auto evs = makeEvents(0, 4);
  EXPECT_EQ(bus.publishBatch(evs.data(), evs.size()), -1);
}

TEST(EventBusBatch, StaticSubscriberMatchesVirtualDelivery)
{
  Bus bus;
  RecordingListener virtualSub;
  PlainRecorder staticSub;
  ASSERT_TRUE(bus.subscribe(&virtualSub, true));
  ASSERT_TRUE(bus.subscribeStatic(&staticSub, true));
  bus.start();

  constexpr int kTotal = 5'000;
  int published = 0;
  while (published < kTotal)
  {
    const int n = std::min(17, kTotal - published);
    const auto evs = makeEvents(published, n);
    bus.publishBatch(evs.data(), static_cast<size_t>(n));
    published += n;
  }
  bus.flush();
  bus.stop();

  ASSERT_EQ(staticSub.values.size(), virtualSub.values.size());
  EXPECT_EQ(staticSub.values, virtualSub.values);
  EXPECT_EQ(staticSub.seqs, virtualSub.seqs);
}

TEST(EventBusBatch, DrainOnStopDeliversTail)
{
  Bus bus;
  RecordingListener a;
  ASSERT_TRUE(bus.subscribe(&a, true));
  bus.enableDrainOnStop();
  bus.start();

  const auto evs = makeEvents(0, 30);
  bus.publishBatch(evs.data(), evs.size());
  bus.flush();
  bus.stop();

  ASSERT_EQ(a.values.size(), 30u);
  const int64_t sum = std::accumulate(a.values.begin(), a.values.end(), int64_t{0});
  EXPECT_EQ(sum, 30 * 29 / 2);
}

}  // namespace
