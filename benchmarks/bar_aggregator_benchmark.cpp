/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/aggregator/bar_aggregator.h"
#include "flox/aggregator/bar_matrix.h"
#include "flox/aggregator/bar_series.h"
#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/multi_timeframe_aggregator.h"
#include "flox/aggregator/policies/tick_bar_policy.h"
#include "flox/aggregator/policies/time_bar_policy.h"
#include "flox/aggregator/policies/volume_bar_policy.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"

#include <benchmark/benchmark.h>
#include <random>

using namespace flox;

// =============================================================================
// TimeBarAggregator benchmarks
// =============================================================================

static void BM_TimeBarAggregator_OnTrade(benchmark::State& state)
{
  constexpr SymbolId SYMBOL = 42;
  constexpr std::chrono::seconds INTERVAL(60);

  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  bus.start();
  aggregator.start();

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(100.0, 110.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  for (auto _ : state)
  {
    TradeEvent event;
    event.trade.symbol = SYMBOL;
    event.trade.price = Price::fromDouble(priceDist(rng));
    event.trade.quantity = Quantity::fromDouble(qtyDist(rng));
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = nowNsMonotonic();

    aggregator.onTrade(event);
  }

  aggregator.stop();
  bus.stop();
}

BENCHMARK(BM_TimeBarAggregator_OnTrade);

// =============================================================================
// TickBarAggregator benchmarks
// =============================================================================

static void BM_TickBarAggregator_OnTrade(benchmark::State& state)
{
  constexpr SymbolId SYMBOL = 42;
  constexpr uint32_t TICK_COUNT = 100;

  BarBus bus;
  bus.enableDrainOnStop();
  TickBarAggregator aggregator(TickBarPolicy(TICK_COUNT), &bus);
  bus.start();
  aggregator.start();

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(100.0, 110.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  for (auto _ : state)
  {
    TradeEvent event;
    event.trade.symbol = SYMBOL;
    event.trade.price = Price::fromDouble(priceDist(rng));
    event.trade.quantity = Quantity::fromDouble(qtyDist(rng));
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = nowNsMonotonic();

    aggregator.onTrade(event);
  }

  aggregator.stop();
  bus.stop();
}

BENCHMARK(BM_TickBarAggregator_OnTrade);

// =============================================================================
// VolumeBarAggregator benchmarks
// =============================================================================

static void BM_VolumeBarAggregator_OnTrade(benchmark::State& state)
{
  constexpr SymbolId SYMBOL = 42;

  BarBus bus;
  bus.enableDrainOnStop();

  VolumeBarAggregator aggregator(VolumeBarPolicy::fromDouble(1000.0), &bus);
  bus.start();
  aggregator.start();

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(100.0, 110.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  for (auto _ : state)
  {
    TradeEvent event;
    event.trade.symbol = SYMBOL;
    event.trade.price = Price::fromDouble(priceDist(rng));
    event.trade.quantity = Quantity::fromDouble(qtyDist(rng));
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = nowNsMonotonic();

    aggregator.onTrade(event);
  }

  aggregator.stop();
  bus.stop();
}

BENCHMARK(BM_VolumeBarAggregator_OnTrade);

// =============================================================================
// Policy shouldClose benchmarks
// =============================================================================

static void BM_TimeBarPolicy_ShouldClose(benchmark::State& state)
{
  TimeBarPolicy policy(std::chrono::seconds(60));

  Bar bar;
  bar.startTime = TimePoint{};
  bar.endTime = TimePoint{};

  TradeEvent event;
  event.trade.exchangeTsNs = 30'000'000'000;  // 30 seconds in

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(policy.shouldClose(event, bar));
  }
}

BENCHMARK(BM_TimeBarPolicy_ShouldClose);

static void BM_TickBarPolicy_ShouldClose(benchmark::State& state)
{
  TickBarPolicy policy(100);

  Bar bar;
  bar.tradeCount = Quantity::fromRaw(50);

  TradeEvent event;

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(policy.shouldClose(event, bar));
  }
}

BENCHMARK(BM_TickBarPolicy_ShouldClose);

static void BM_VolumeBarPolicy_ShouldClose(benchmark::State& state)
{
  VolumeBarPolicy policy = VolumeBarPolicy::fromDouble(1000.0);

  Bar bar;
  bar.volume = Volume::fromDouble(500.0);

  TradeEvent event;

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(policy.shouldClose(event, bar));
  }
}

BENCHMARK(BM_VolumeBarPolicy_ShouldClose);

// =============================================================================
// MultiTimeframeAggregator benchmarks
// =============================================================================

static void BM_MultiTimeframeAggregator_4TF(benchmark::State& state)
{
  BarBus bus;
  bus.enableDrainOnStop();
  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addTimeInterval(std::chrono::seconds(60));
  aggregator.addTimeInterval(std::chrono::seconds(300));
  aggregator.addTimeInterval(std::chrono::seconds(900));
  aggregator.addTimeInterval(std::chrono::seconds(3600));

  bus.start();
  aggregator.start();

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(100.0, 110.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  for (auto _ : state)
  {
    TradeEvent event;
    event.trade.symbol = 42;
    event.trade.price = Price::fromDouble(priceDist(rng));
    event.trade.quantity = Quantity::fromDouble(qtyDist(rng));
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = nowNsMonotonic();

    aggregator.onTrade(event);
  }

  aggregator.stop();
  bus.stop();
}

BENCHMARK(BM_MultiTimeframeAggregator_4TF);

// =============================================================================
// MultiTimeframeAggregator scaling benchmarks
// =============================================================================

template <size_t NumTimeframes>
static void BM_MultiTimeframeAggregator_Scaling(benchmark::State& state)
{
  BarBus bus;
  bus.enableDrainOnStop();
  MultiTimeframeAggregator<NumTimeframes> aggregator(&bus);

  // Add timeframes - mix of Time, Tick, Volume policies
  for (size_t i = 0; i < NumTimeframes; ++i)
  {
    switch (i % 3)
    {
      case 0:
        aggregator.addTimeInterval(std::chrono::seconds(60 + i * 30));
        break;
      case 1:
        aggregator.addTickInterval(100 + i * 10);
        break;
      case 2:
        aggregator.addVolumeInterval(1000.0 + i * 500.0);
        break;
    }
  }

  bus.start();
  aggregator.start();

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(100.0, 110.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  for (auto _ : state)
  {
    TradeEvent event;
    event.trade.symbol = 42;
    event.trade.price = Price::fromDouble(priceDist(rng));
    event.trade.quantity = Quantity::fromDouble(qtyDist(rng));
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = nowNsMonotonic();

    aggregator.onTrade(event);
  }

  state.SetItemsProcessed(state.iterations() * NumTimeframes);
  state.counters["timeframes"] = NumTimeframes;
  state.counters["ns_per_tf"] =
      benchmark::Counter(state.iterations() * NumTimeframes,
                         benchmark::Counter::kIsRate | benchmark::Counter::kInvert);

  aggregator.stop();
  bus.stop();
}

BENCHMARK(BM_MultiTimeframeAggregator_Scaling<1>)->Name("BM_MTF_Scaling/1");
BENCHMARK(BM_MultiTimeframeAggregator_Scaling<2>)->Name("BM_MTF_Scaling/2");
BENCHMARK(BM_MultiTimeframeAggregator_Scaling<4>)->Name("BM_MTF_Scaling/4");
BENCHMARK(BM_MultiTimeframeAggregator_Scaling<8>)->Name("BM_MTF_Scaling/8");
BENCHMARK(BM_MultiTimeframeAggregator_Scaling<16>)->Name("BM_MTF_Scaling/16");
BENCHMARK(BM_MultiTimeframeAggregator_Scaling<32>)->Name("BM_MTF_Scaling/32");
BENCHMARK(BM_MultiTimeframeAggregator_Scaling<64>)->Name("BM_MTF_Scaling/64");
BENCHMARK(BM_MultiTimeframeAggregator_Scaling<128>)->Name("BM_MTF_Scaling/128");
BENCHMARK(BM_MultiTimeframeAggregator_Scaling<256>)->Name("BM_MTF_Scaling/256");
BENCHMARK(BM_MultiTimeframeAggregator_Scaling<512>)->Name("BM_MTF_Scaling/512");
BENCHMARK(BM_MultiTimeframeAggregator_Scaling<1024>)->Name("BM_MTF_Scaling/1024");

// Compare with N separate aggregators (no std::variant overhead)
template <size_t NumAggregators>
static void BM_SeparateAggregators_Scaling(benchmark::State& state)
{
  BarBus bus;
  bus.enableDrainOnStop();

  // Create N separate TimeBarAggregators
  std::array<std::unique_ptr<TimeBarAggregator>, NumAggregators> aggregators;
  for (size_t i = 0; i < NumAggregators; ++i)
  {
    aggregators[i] = std::make_unique<TimeBarAggregator>(
        TimeBarPolicy(std::chrono::seconds(60 + i * 30)), &bus);
  }

  bus.start();
  for (auto& agg : aggregators)
  {
    agg->start();
  }

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(100.0, 110.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  for (auto _ : state)
  {
    TradeEvent event;
    event.trade.symbol = 42;
    event.trade.price = Price::fromDouble(priceDist(rng));
    event.trade.quantity = Quantity::fromDouble(qtyDist(rng));
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = nowNsMonotonic();

    for (auto& agg : aggregators)
    {
      agg->onTrade(event);
    }
  }

  state.SetItemsProcessed(state.iterations() * NumAggregators);
  state.counters["aggregators"] = NumAggregators;
  state.counters["ns_per_agg"] =
      benchmark::Counter(state.iterations() * NumAggregators,
                         benchmark::Counter::kIsRate | benchmark::Counter::kInvert);

  for (auto& agg : aggregators)
  {
    agg->stop();
  }
  bus.stop();
}

BENCHMARK(BM_SeparateAggregators_Scaling<1>)->Name("BM_Separate_Scaling/1");
BENCHMARK(BM_SeparateAggregators_Scaling<2>)->Name("BM_Separate_Scaling/2");
BENCHMARK(BM_SeparateAggregators_Scaling<4>)->Name("BM_Separate_Scaling/4");
BENCHMARK(BM_SeparateAggregators_Scaling<8>)->Name("BM_Separate_Scaling/8");
BENCHMARK(BM_SeparateAggregators_Scaling<16>)->Name("BM_Separate_Scaling/16");
BENCHMARK(BM_SeparateAggregators_Scaling<32>)->Name("BM_Separate_Scaling/32");
BENCHMARK(BM_SeparateAggregators_Scaling<64>)->Name("BM_Separate_Scaling/64");
BENCHMARK(BM_SeparateAggregators_Scaling<128>)->Name("BM_Separate_Scaling/128");
BENCHMARK(BM_SeparateAggregators_Scaling<256>)->Name("BM_Separate_Scaling/256");
BENCHMARK(BM_SeparateAggregators_Scaling<512>)->Name("BM_Separate_Scaling/512");
BENCHMARK(BM_SeparateAggregators_Scaling<1024>)->Name("BM_Separate_Scaling/1024");

// =============================================================================
// Homogeneous benchmarks (all same policy type)
// =============================================================================

// All TimeBarPolicy (homogeneous)
template <size_t NumTimeframes>
static void BM_MTF_TimeOnly(benchmark::State& state)
{
  BarBus bus;
  bus.enableDrainOnStop();
  MultiTimeframeAggregator<NumTimeframes> aggregator(&bus);

  for (size_t i = 0; i < NumTimeframes; ++i)
  {
    aggregator.addTimeInterval(std::chrono::seconds(60 + i * 30));
  }

  bus.start();
  aggregator.start();

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(100.0, 110.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  for (auto _ : state)
  {
    TradeEvent event;
    event.trade.symbol = 42;
    event.trade.price = Price::fromDouble(priceDist(rng));
    event.trade.quantity = Quantity::fromDouble(qtyDist(rng));
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = nowNsMonotonic();

    aggregator.onTrade(event);
  }

  state.SetItemsProcessed(state.iterations() * NumTimeframes);
  state.counters["timeframes"] = NumTimeframes;
  state.counters["ns_per_tf"] =
      benchmark::Counter(state.iterations() * NumTimeframes,
                         benchmark::Counter::kIsRate | benchmark::Counter::kInvert);

  aggregator.stop();
  bus.stop();
}

BENCHMARK(BM_MTF_TimeOnly<4>)->Name("BM_MTF_Homogeneous_Time/4");
BENCHMARK(BM_MTF_TimeOnly<8>)->Name("BM_MTF_Homogeneous_Time/8");
BENCHMARK(BM_MTF_TimeOnly<16>)->Name("BM_MTF_Homogeneous_Time/16");
BENCHMARK(BM_MTF_TimeOnly<32>)->Name("BM_MTF_Homogeneous_Time/32");

// All VolumeBarPolicy (homogeneous)
template <size_t NumTimeframes>
static void BM_MTF_VolumeOnly(benchmark::State& state)
{
  BarBus bus;
  bus.enableDrainOnStop();
  MultiTimeframeAggregator<NumTimeframes> aggregator(&bus);

  for (size_t i = 0; i < NumTimeframes; ++i)
  {
    aggregator.addVolumeInterval(1000.0 + i * 500.0);
  }

  bus.start();
  aggregator.start();

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(100.0, 110.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  for (auto _ : state)
  {
    TradeEvent event;
    event.trade.symbol = 42;
    event.trade.price = Price::fromDouble(priceDist(rng));
    event.trade.quantity = Quantity::fromDouble(qtyDist(rng));
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = nowNsMonotonic();

    aggregator.onTrade(event);
  }

  state.SetItemsProcessed(state.iterations() * NumTimeframes);
  state.counters["timeframes"] = NumTimeframes;
  state.counters["ns_per_tf"] =
      benchmark::Counter(state.iterations() * NumTimeframes,
                         benchmark::Counter::kIsRate | benchmark::Counter::kInvert);

  aggregator.stop();
  bus.stop();
}

BENCHMARK(BM_MTF_VolumeOnly<4>)->Name("BM_MTF_Homogeneous_Volume/4");
BENCHMARK(BM_MTF_VolumeOnly<8>)->Name("BM_MTF_Homogeneous_Volume/8");
BENCHMARK(BM_MTF_VolumeOnly<16>)->Name("BM_MTF_Homogeneous_Volume/16");
BENCHMARK(BM_MTF_VolumeOnly<32>)->Name("BM_MTF_Homogeneous_Volume/32");

// =============================================================================
// BarSeries benchmarks
// =============================================================================

static void BM_BarSeries_Push(benchmark::State& state)
{
  BarSeries<256> series;

  Bar bar;
  bar.open = Price::fromDouble(100.0);
  bar.high = Price::fromDouble(101.0);
  bar.low = Price::fromDouble(99.0);
  bar.close = Price::fromDouble(100.5);
  bar.volume = Volume::fromDouble(1000.0);

  for (auto _ : state)
  {
    series.push(bar);
  }
}

BENCHMARK(BM_BarSeries_Push);

static void BM_BarSeries_RandomAccess(benchmark::State& state)
{
  BarSeries<256> series;

  Bar bar;
  bar.open = Price::fromDouble(100.0);
  for (size_t i = 0; i < 256; ++i)
  {
    series.push(bar);
  }

  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> idxDist(0, 255);

  for (auto _ : state)
  {
    size_t idx = idxDist(rng);
    const Bar& b = series[idx];
    benchmark::DoNotOptimize(&b);
  }
}

BENCHMARK(BM_BarSeries_RandomAccess);

static void BM_BarSeries_Iterate(benchmark::State& state)
{
  BarSeries<256> series;

  Bar bar;
  bar.open = Price::fromDouble(100.0);
  for (size_t i = 0; i < 256; ++i)
  {
    series.push(bar);
  }

  for (auto _ : state)
  {
    Price sum{};
    for (const auto& b : series)
    {
      sum = sum + b.close;
    }
    benchmark::DoNotOptimize(sum);
  }
}

BENCHMARK(BM_BarSeries_Iterate);

// =============================================================================
// BarMatrix benchmarks
// =============================================================================

static void BM_BarMatrix_RandomAccess(benchmark::State& state)
{
  BarMatrix<64, 4, 256> matrix;

  std::array<TimeframeId, 4> timeframes = {
      TimeframeId::time(std::chrono::seconds(60)),
      TimeframeId::time(std::chrono::seconds(300)),
      TimeframeId::time(std::chrono::seconds(900)),
      TimeframeId::time(std::chrono::seconds(3600)),
  };
  matrix.configure(timeframes);

  // Populate with data
  Bar bar;
  bar.open = Price::fromDouble(100.0);
  for (SymbolId sym = 0; sym < 64; ++sym)
  {
    for (size_t tf = 0; tf < 4; ++tf)
    {
      std::vector<Bar> history(256, bar);
      matrix.warmup(sym, timeframes[tf], history);
    }
  }

  std::mt19937 rng(42);
  std::uniform_int_distribution<SymbolId> symDist(0, 63);
  std::uniform_int_distribution<size_t> tfDist(0, 3);
  std::uniform_int_distribution<size_t> idxDist(0, 255);

  for (auto _ : state)
  {
    SymbolId sym = symDist(rng);
    size_t tf = tfDist(rng);
    size_t idx = idxDist(rng);
    benchmark::DoNotOptimize(matrix.bar(sym, tf, idx));
  }
}

BENCHMARK(BM_BarMatrix_RandomAccess);

static void BM_BarMatrix_SequentialAccess(benchmark::State& state)
{
  BarMatrix<64, 4, 256> matrix;

  std::array<TimeframeId, 4> timeframes = {
      TimeframeId::time(std::chrono::seconds(60)),
      TimeframeId::time(std::chrono::seconds(300)),
      TimeframeId::time(std::chrono::seconds(900)),
      TimeframeId::time(std::chrono::seconds(3600)),
  };
  matrix.configure(timeframes);

  Bar bar;
  bar.open = Price::fromDouble(100.0);
  for (SymbolId sym = 0; sym < 64; ++sym)
  {
    for (size_t tf = 0; tf < 4; ++tf)
    {
      std::vector<Bar> history(256, bar);
      matrix.warmup(sym, timeframes[tf], history);
    }
  }

  for (auto _ : state)
  {
    Price sum{};
    for (SymbolId sym = 0; sym < 64; ++sym)
    {
      for (size_t tf = 0; tf < 4; ++tf)
      {
        for (size_t idx = 0; idx < 10; ++idx)
        {
          const auto* b = matrix.bar(sym, tf, idx);
          if (b)
          {
            sum = sum + b->close;
          }
        }
      }
    }
    benchmark::DoNotOptimize(sum);
  }
}

BENCHMARK(BM_BarMatrix_SequentialAccess);

// =============================================================================
// Multi-symbol benchmarks
// =============================================================================

static void BM_TimeBarAggregator_100Symbols(benchmark::State& state)
{
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(std::chrono::seconds(60)), &bus);
  bus.start();
  aggregator.start();

  std::mt19937 rng(42);
  std::uniform_int_distribution<SymbolId> symDist(0, 99);
  std::uniform_real_distribution<> priceDist(100.0, 110.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  for (auto _ : state)
  {
    TradeEvent event;
    event.trade.symbol = symDist(rng);
    event.trade.price = Price::fromDouble(priceDist(rng));
    event.trade.quantity = Quantity::fromDouble(qtyDist(rng));
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = nowNsMonotonic();

    aggregator.onTrade(event);
  }

  aggregator.stop();
  bus.stop();
}

BENCHMARK(BM_TimeBarAggregator_100Symbols);

BENCHMARK_MAIN();
