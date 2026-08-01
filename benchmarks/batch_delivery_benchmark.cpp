/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The two delivery fast paths, measured against their baselines:
//
//   1. EventBus::publishBatch vs per-event publish() -- one sequence
//      reservation, one wrap/reclaim wait, and one release fence per batch
//      instead of per event. One producer thread, 1M TradeEvents, N required
//      consumers, flush-to-complete.
//   2. StrategyPump (monomorphic replay) vs virtual delivery of the same
//      handler -- the research-sweep fast path where the strategy inlines
//      into the replay loop and its state stays in registers.
//
// On macOS, spinning multi-thread benchmarks are invalid without the
// real-time scheduling class: the scheduler preempts the consumers and
// becomes a confounder, not just noise. Set FLOX_BENCH_RT=1 to opt in
// (no-op elsewhere; on Linux use taskset/isolcpus instead).

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

#include "flox/backtest/strategy_pump.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/util/eventing/event_bus.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif

namespace
{

using namespace flox;

constexpr size_t kSourceEvents = 4096;
constexpr size_t kBusEvents = 1 << 20;
constexpr size_t kTapeEvents = 1 << 20;
constexpr size_t kChunk = 256;

void makeSelfRealtime()
{
#ifdef __APPLE__
  static const bool enabled = []
  {
    const char* v = std::getenv("FLOX_BENCH_RT");
    return v != nullptr && v[0] == '1';
  }();
  if (!enabled)
  {
    return;
  }
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  const auto nsToAbs = [&](uint64_t ns)
  { return static_cast<uint32_t>(ns * tb.denom / tb.numer); };
  thread_time_constraint_policy_data_t tc;
  tc.period = 0;
  tc.computation = nsToAbs(5'000'000);
  tc.constraint = nsToAbs(10'000'000);
  tc.preemptible = 1;
  thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                    reinterpret_cast<thread_policy_t>(&tc),
                    THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#endif
}

std::vector<TradeEvent> makeEvents(size_t count, uint64_t seed)
{
  std::vector<TradeEvent> events(count);
  std::mt19937_64 rng(seed);
  int64_t price = 10'000'000;
  std::uniform_int_distribution<int64_t> step(-3000, 3000);
  std::uniform_int_distribution<int64_t> qty(1'000, 500'000);
  for (auto& ev : events)
  {
    price += step(rng);
    ev.trade.symbol = 1;
    ev.trade.price = Price::fromRaw(price);
    ev.trade.quantity = Quantity::fromRaw(qty(rng));
  }
  return events;
}

// --- bus: per-event vs batch -------------------------------------------------

class SummingSub final : public IMarketDataSubscriber
{
 public:
  void onTrade(const TradeEvent& ev) override
  {
    if (!_rtApplied)
    {
      _rtApplied = true;
      makeSelfRealtime();
    }
    acc += ev.trade.price.raw();
  }
  SubscriberId id() const override { return 1; }
  int64_t acc = 0;

 private:
  bool _rtApplied = false;
};

template <bool UseBatch>
void busSession(benchmark::State& state)
{
  const auto events = makeEvents(kSourceEvents, 0xBEEFull);
  const size_t numConsumers = static_cast<size_t>(state.range(0));
  for (auto _ : state)
  {
    EventBus<TradeEvent, 4096, 8> bus;
    std::vector<std::unique_ptr<SummingSub>> subs;
    for (size_t i = 0; i < numConsumers; ++i)
    {
      subs.push_back(std::make_unique<SummingSub>());
      bus.subscribe(subs.back().get(), true);
    }
    bus.start();
    makeSelfRealtime();
    size_t ei = 0;
    if constexpr (UseBatch)
    {
      for (size_t i = 0; i < kBusEvents;)
      {
        const size_t n = std::min({kChunk, kBusEvents - i, kSourceEvents - ei});
        bus.publishBatch(&events[ei], n);
        ei = (ei + n) & (kSourceEvents - 1);
        i += n;
      }
    }
    else
    {
      for (size_t i = 0; i < kBusEvents; ++i)
      {
        bus.publish(events[ei]);
        ei = (ei + 1) & (kSourceEvents - 1);
      }
    }
    bus.flush();
    bus.stop();
    for (auto& s : subs)
    {
      benchmark::DoNotOptimize(s->acc);
    }
  }
  state.SetItemsProcessed(state.iterations() * kBusEvents);
}

void BM_Bus_PublishPerEvent(benchmark::State& state) { busSession<false>(state); }
BENCHMARK(BM_Bus_PublishPerEvent)->Arg(1)->Arg(4)->Unit(benchmark::kMillisecond);

void BM_Bus_PublishBatch(benchmark::State& state) { busSession<true>(state); }
BENCHMARK(BM_Bus_PublishBatch)->Arg(1)->Arg(4)->Unit(benchmark::kMillisecond);

// --- pump: virtual vs monomorphic --------------------------------------------

struct EmaCross
{
  int64_t emaFast = 0;
  int64_t emaSlow = 0;
  int64_t position = 0;
  int64_t pnl = 0;
  int64_t lastPrice = 0;

  void onTrade(const TradeEvent& ev)
  {
    const int64_t p = ev.trade.price.raw();
    emaFast += (p - emaFast) >> 4;
    emaSlow += (p - emaSlow) >> 7;
    if (position != 0)
    {
      pnl += position * (p - lastPrice);
    }
    lastPrice = p;
    position = emaFast > emaSlow ? 1 : -1;
  }
};

struct VirtualEmaCross final : IMarketDataSubscriber
{
  EmaCross impl;
  void onTrade(const TradeEvent& ev) override { impl.onTrade(ev); }
  SubscriberId id() const override { return 1; }
};

std::unique_ptr<IMarketDataSubscriber> makeVirtualEma()
{
  return std::make_unique<VirtualEmaCross>();
}

// A second live factory keeps the call site polymorphic: with a single
// possible target, LTO devirtualizes the "virtual" baseline and the
// comparison silently measures mono against mono.
struct VirtualNoop final : IMarketDataSubscriber
{
  void onTrade(const TradeEvent& ev) override { acc += ev.trade.price.raw(); }
  SubscriberId id() const override { return 2; }
  int64_t acc = 0;
};

std::unique_ptr<IMarketDataSubscriber> makeVirtualNoop()
{
  return std::make_unique<VirtualNoop>();
}

using StrategyFactory = std::unique_ptr<IMarketDataSubscriber> (*)();
const StrategyFactory kFactories[2] = {&makeVirtualEma, &makeVirtualNoop};

size_t opaqueIndex(size_t value)
{
  size_t idx = value;
  benchmark::DoNotOptimize(idx);
  return idx;
}

void BM_Pump_Virtual(benchmark::State& state)
{
  const auto tape = makeEvents(kTapeEvents, 0x7A9Eull);
  auto strategy = kFactories[opaqueIndex(0)]();
  for (auto _ : state)
  {
    for (const TradeEvent& ev : tape)
    {
      strategy->onTrade(ev);
    }
  }
  state.SetItemsProcessed(state.iterations() * kTapeEvents);
}
BENCHMARK(BM_Pump_Virtual);

void BM_Pump_Mono(benchmark::State& state)
{
  const auto tape = makeEvents(kTapeEvents, 0x7A9Eull);
  EmaCross strategy;
  StrategyPump<EmaCross> pump(strategy);
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(pump.runTrades(tape).trades);
  }
  benchmark::DoNotOptimize(strategy);
  state.SetItemsProcessed(state.iterations() * kTapeEvents);
}
BENCHMARK(BM_Pump_Mono);

}  // namespace

BENCHMARK_MAIN();
