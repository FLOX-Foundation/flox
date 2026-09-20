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
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "flox/util/concurrency/thread_body.h"
#include "flox/util/eventing/event_bus.h"
#include "flox/util/eventing/wake_set.h"
#include "flox/util/performance/busy_backoff.h"

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

int64_t nowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Stamps the moment the handler was entered, so the publisher can read how
// long the consumer took to notice -- from the far end of its backoff, not
// from a spin it never left.
class StampingListener : public BenchEvent::Listener
{
 public:
  void onEvent(const BenchEvent&) override { gotNs.store(nowNs(), std::memory_order_release); }
  std::atomic<int64_t> gotNs{0};
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
    publishers.push_back(makeThread("bench.event_bus.publisher", [&bus, &running, &totalPublished]
                                    {
      flox::BenchEvent event{};
      while (running.load(std::memory_order_relaxed))
      {
        bus.publish(event);
        totalPublished.fetch_add(1, std::memory_order_relaxed);
      } }));
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

// =============================================================================
// Wake-from-idle latency: publish -> handler entry, after the consumer has
// been idle long enough to reach the far end of its backoff. This is the
// number a parked consumer would have to beat, and the one an active wait
// buys with a burning core.
// =============================================================================

static void idleWakeLatency(benchmark::State& state, flox::ConsumerWaitMode mode)
{
  flox::EventBus<flox::BenchEvent, 4096, 4> bus;
  StampingListener listener;

  bus.subscribe(&listener, /*required=*/true, mode);
  bus.start();

  flox::BenchEvent event{};
  int64_t total = 0;
  int64_t worst = 0;
  int64_t n = 0;

  for (auto _ : state)
  {
    // Long enough for ADAPTIVE to walk through its spins and yields into the
    // sleeping tiers.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    listener.gotNs.store(0, std::memory_order_release);

    const int64_t t0 = nowNs();
    bus.publish(event);
    int64_t got = 0;
    while ((got = listener.gotNs.load(std::memory_order_acquire)) == 0)
    {
    }
    const int64_t d = got - t0;
    total += d;
    worst = d > worst ? d : worst;
    ++n;
  }

  bus.stop();

  state.counters["wake_ns"] = n != 0 ? double(total) / double(n) : 0.0;
  state.counters["wake_max_ns"] = double(worst);
}

// Active waiting: the consumer is somewhere in its backoff when the event
// lands, so this is what the burning core buys.
static void BM_EventBus_IdleWakeLatency(benchmark::State& state)
{
  idleWakeLatency(state, flox::ConsumerWaitMode::ACTIVE);
}
BENCHMARK(BM_EventBus_IdleWakeLatency)->Iterations(300)->UseRealTime();

// Parked: the consumer is blocked, and the publisher has to wake it. The
// difference between the two is the price of the mode, and it is the number
// to look at before making parking anybody's default.
static void BM_EventBus_IdleWakeLatency_Parked(benchmark::State& state)
{
  idleWakeLatency(state, flox::ConsumerWaitMode::PARKED);
}
BENCHMARK(BM_EventBus_IdleWakeLatency_Parked)->Iterations(300)->UseRealTime();

// =============================================================================
// One thread over N buses: wake-from-idle latency, and what the thread costs
// while every ring is quiet.
//
// Parking is per consumer and per bus, so it does nothing for the shape a
// process with hundreds of rings actually takes: one thread stepping all of
// them. Such a thread cannot block on any single bus, so before the shared
// wait point it had no choice but a backoff -- a burning core, which is the
// cost parking exists to remove. The two variants below are that thread with
// and without a WakeSet; wake_ns is the price of the set, and the difference
// in idle CPU is what it buys.
// =============================================================================

static void fanoutWakeLatency(benchmark::State& state, bool useWakeSet)
{
  using FanBus = flox::EventBus<flox::BenchEvent, 4096, 4>;
  const int nBuses = static_cast<int>(state.range(0));

  flox::WakeSet set;
  std::vector<std::unique_ptr<FanBus>> buses;
  std::vector<std::unique_ptr<StampingListener>> sinks;
  for (int b = 0; b < nBuses; ++b)
  {
    buses.push_back(std::make_unique<FanBus>());
    sinks.push_back(std::make_unique<StampingListener>());
    buses[b]->setOwnConsumerThreads(false);
    if (useWakeSet)
    {
      buses[b]->setWakeSet(&set);
    }
    buses[b]->subscribe(sinks[b].get());
    buses[b]->start();
  }

  std::atomic<bool> running{true};
  auto driver = makeThread(
      "bench.event_bus.fanout.driver",
      [&buses, &set, &running, useWakeSet]
      {
        flox::BusyBackoff backoff;
        const auto pending = [&buses]
        {
          for (auto& bus : buses)
          {
            for (uint32_t i = 0; i < bus->consumerCount(); ++i)
            {
              if (bus->consumerHasPending(i))
              {
                return true;
              }
            }
          }
          return false;
        };
        while (running.load(std::memory_order_acquire))
        {
          bool any = false;
          for (auto& bus : buses)
          {
            for (uint32_t i = 0; i < bus->consumerCount(); ++i)
            {
              any |= bus->pollConsumer(i);
            }
          }
          if (any)
          {
            backoff.reset();
            continue;
          }
          if (useWakeSet)
          {
            set.parkUnless([&]
                           { return !running.load(std::memory_order_acquire) || pending(); });
          }
          else
          {
            backoff.pause();
          }
        }
      });

  flox::BenchEvent event{};
  int64_t total = 0;
  int64_t worst = 0;
  int64_t n = 0;
  int b = 0;

  for (auto _ : state)
  {
    // Long enough for the driver to be asleep on the set, or at the far end
    // of its backoff.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto& sink = *sinks[b];
    sink.gotNs.store(0, std::memory_order_release);

    const int64_t t0 = nowNs();
    buses[b]->publish(event);
    int64_t got = 0;
    while ((got = sink.gotNs.load(std::memory_order_acquire)) == 0)
    {
    }
    const int64_t d = got - t0;
    total += d;
    worst = d > worst ? d : worst;
    ++n;
    b = (b + 1) % nBuses;
  }

  running.store(false, std::memory_order_release);
  set.wake();
  driver.join();
  for (auto& bus : buses)
  {
    bus->stop();
  }

  state.counters["wake_ns"] = n != 0 ? double(total) / double(n) : 0.0;
  state.counters["wake_max_ns"] = double(worst);
}

// The only wait such a thread had before: spin, yield, short sleeps. It never
// blocks, so an idle process pays for it in full.
static void BM_EventBus_FanoutWakeLatency_Backoff(benchmark::State& state)
{
  fanoutWakeLatency(state, /*useWakeSet=*/false);
}
BENCHMARK(BM_EventBus_FanoutWakeLatency_Backoff)->Arg(3)->Arg(16)->Iterations(300)->UseRealTime();

// The same thread on a shared wait point: it blocks, and whichever bus is
// published into wakes it.
static void BM_EventBus_FanoutWakeLatency_WakeSet(benchmark::State& state)
{
  fanoutWakeLatency(state, /*useWakeSet=*/true);
}
BENCHMARK(BM_EventBus_FanoutWakeLatency_WakeSet)->Arg(3)->Arg(16)->Iterations(300)->UseRealTime();

// Publish cost with a parked consumer attached: the publisher now has a
// wake-up to do. Compared against BM_EventBus_PublishLatency, which has an
// active consumer and never wakes anybody.
static void BM_EventBus_PublishLatency_ParkedConsumer(benchmark::State& state)
{
  flox::EventBus<flox::BenchEvent, 4096, 4> bus;
  NoOpListener listener;

  bus.subscribe(&listener, /*required=*/true, flox::ConsumerWaitMode::PARKED);
  bus.start();

  flox::BenchEvent event{};
  for (auto _ : state)
  {
    bus.publish(event);
  }

  bus.stop();
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EventBus_PublishLatency_ParkedConsumer)->Iterations(1'000'000);

BENCHMARK_MAIN();
