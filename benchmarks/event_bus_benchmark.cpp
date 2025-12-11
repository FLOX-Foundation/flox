/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <benchmark/benchmark.h>
#include <atomic>
#include <thread>
#include <vector>

#include "flox/util/eventing/event_bus.h"

namespace flox
{

struct BenchEvent
{
  using Listener = struct IBenchListener
  {
    virtual ~IBenchListener() = default;
    virtual void onEvent(const BenchEvent& e) = 0;
  };

  int64_t data[8]{};  // 64 bytes payload
  uint64_t tickSequence{0};
};

template <>
struct EventDispatcher<BenchEvent>
{
  static void dispatch(const BenchEvent& event, BenchEvent::Listener& listener)
  {
    listener.onEvent(event);
  }
};

}  // namespace flox

namespace
{

using namespace flox;

class NoOpListener : public BenchEvent::Listener
{
 public:
  void onEvent(const BenchEvent&) override
  {
    ++count;
  }
  std::atomic<int64_t> count{0};
};

}  // namespace

// =============================================================================
// Single-threaded publish latency
// =============================================================================

static void BM_EventBus_PublishLatency(benchmark::State& state)
{
  flox::EventBus<flox::BenchEvent, 4096, 4> bus;
  NoOpListener listener;

  bus.subscribe(&listener);
  bus.start();

  flox::BenchEvent event{};

  for (auto _ : state)
  {
    bus.publish(event);
  }

  bus.flush();
  bus.stop();

  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EventBus_PublishLatency)->Iterations(1'000'000);

// =============================================================================
// Publish throughput with single consumer
// =============================================================================

static void BM_EventBus_SingleConsumerThroughput(benchmark::State& state)
{
  flox::EventBus<flox::BenchEvent, 4096, 4> bus;
  NoOpListener listener;

  bus.subscribe(&listener);
  bus.start();

  flox::BenchEvent event{};
  int64_t totalPublished = 0;

  for (auto _ : state)
  {
    for (int i = 0; i < 1000; ++i)
    {
      bus.publish(event);
      ++totalPublished;
    }
  }

  bus.flush();
  bus.stop();

  state.SetItemsProcessed(totalPublished);
  state.SetBytesProcessed(totalPublished * sizeof(flox::BenchEvent));
}
BENCHMARK(BM_EventBus_SingleConsumerThroughput);

// =============================================================================
// Publish throughput with multiple consumers
// =============================================================================

static void BM_EventBus_MultiConsumerThroughput(benchmark::State& state)
{
  const int numConsumers = state.range(0);

  flox::EventBus<flox::BenchEvent, 4096, 8> bus;
  std::vector<std::unique_ptr<NoOpListener>> listeners;

  for (int i = 0; i < numConsumers; ++i)
  {
    listeners.push_back(std::make_unique<NoOpListener>());
    bus.subscribe(listeners.back().get());
  }

  bus.start();

  flox::BenchEvent event{};
  int64_t totalPublished = 0;

  for (auto _ : state)
  {
    for (int i = 0; i < 1000; ++i)
    {
      bus.publish(event);
      ++totalPublished;
    }
  }

  bus.flush();
  bus.stop();

  state.SetItemsProcessed(totalPublished);
}
BENCHMARK(BM_EventBus_MultiConsumerThroughput)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

// =============================================================================
// Concurrent publishers
// =============================================================================

static void BM_EventBus_ConcurrentPublishers(benchmark::State& state)
{
  const int numPublishers = state.range(0);

  flox::EventBus<flox::BenchEvent, 8192, 4> bus;
  NoOpListener listener;

  bus.subscribe(&listener);
  bus.start();

  std::atomic<bool> running{true};
  std::atomic<int64_t> totalPublished{0};

  std::vector<std::thread> publishers;
  for (int i = 0; i < numPublishers; ++i)
  {
    publishers.emplace_back([&bus, &running, &totalPublished]
                            {
      flox::BenchEvent event{};
      while (running.load(std::memory_order_relaxed))
      {
        bus.publish(event);
        totalPublished.fetch_add(1, std::memory_order_relaxed);
      } });
  }

  for (auto _ : state)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }

  running.store(false, std::memory_order_relaxed);

  for (auto& t : publishers)
  {
    t.join();
  }

  bus.flush();
  bus.stop();

  state.SetItemsProcessed(totalPublished.load());
}
BENCHMARK(BM_EventBus_ConcurrentPublishers)->Arg(1)->Arg(2)->Arg(4);

// =============================================================================
// tryPublish latency (no backpressure)
// =============================================================================

static void BM_EventBus_TryPublishLatency(benchmark::State& state)
{
  flox::EventBus<flox::BenchEvent, 4096, 4> bus;
  NoOpListener listener;

  bus.subscribe(&listener);
  bus.start();

  flox::BenchEvent event{};

  for (auto _ : state)
  {
    auto [result, seq] = bus.tryPublish(event, std::chrono::microseconds{1000});
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(seq);
  }

  bus.flush();
  bus.stop();

  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EventBus_TryPublishLatency)->Iterations(1'000'000);

// =============================================================================
// Buffer wrap-around performance
// =============================================================================

static void BM_EventBus_WrapAround(benchmark::State& state)
{
  const size_t bufferSize = state.range(0);

  // Use different buffer sizes
  flox::EventBus<flox::BenchEvent, 256, 4> bus256;
  flox::EventBus<flox::BenchEvent, 1024, 4> bus1024;
  flox::EventBus<flox::BenchEvent, 4096, 4> bus4096;

  NoOpListener listener;

  if (bufferSize == 256)
  {
    bus256.subscribe(&listener);
    bus256.start();

    flox::BenchEvent event{};
    for (auto _ : state)
    {
      for (int i = 0; i < 1000; ++i)
      {
        bus256.publish(event);
      }
    }
    bus256.flush();
    bus256.stop();
  }
  else if (bufferSize == 1024)
  {
    bus1024.subscribe(&listener);
    bus1024.start();

    flox::BenchEvent event{};
    for (auto _ : state)
    {
      for (int i = 0; i < 1000; ++i)
      {
        bus1024.publish(event);
      }
    }
    bus1024.flush();
    bus1024.stop();
  }
  else
  {
    bus4096.subscribe(&listener);
    bus4096.start();

    flox::BenchEvent event{};
    for (auto _ : state)
    {
      for (int i = 0; i < 1000; ++i)
      {
        bus4096.publish(event);
      }
    }
    bus4096.flush();
    bus4096.stop();
  }

  state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_EventBus_WrapAround)->Arg(256)->Arg(1024)->Arg(4096);

// =============================================================================
// End-to-end latency (publish -> consume)
// =============================================================================

static void BM_EventBus_EndToEndLatency(benchmark::State& state)
{
  flox::EventBus<flox::BenchEvent, 4096, 4> bus;
  NoOpListener listener;

  bus.subscribe(&listener);
  bus.start();

  flox::BenchEvent event{};

  for (auto _ : state)
  {
    auto seq = bus.publish(event);
    bus.waitConsumed(seq);
  }

  bus.stop();

  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EventBus_EndToEndLatency)->Iterations(100'000);

BENCHMARK_MAIN();
