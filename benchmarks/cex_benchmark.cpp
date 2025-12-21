/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/book/composite_book_matrix.h"
#include "flox/position/aggregated_position_tracker.h"
#include "flox/util/sync/exchange_clock_sync.h"

#include <benchmark/benchmark.h>
#include <random>
#include <thread>

using namespace flox;

// ============================================================================
// CompositeBookMatrix Benchmarks
// ============================================================================

static void BM_CompositeBookMatrix_BestBid(benchmark::State& state)
{
  const size_t numExchanges = state.range(0);
  CompositeBookMatrix<8> matrix;

  std::pmr::monotonic_buffer_resource pool(1024 * 1024);
  BookUpdateEvent ev(&pool);

  // Pre-populate with book data from multiple exchanges
  for (size_t ex = 0; ex < numExchanges; ++ex)
  {
    ev.update.symbol = 1;
    ev.sourceExchange = static_cast<ExchangeId>(ex);
    ev.update.bids.clear();
    ev.update.asks.clear();
    ev.update.bids.emplace_back(Price::fromRaw(50000 * 1'000'000LL + ex * 100), Quantity::fromRaw(10 * 1'000'000LL));
    ev.update.asks.emplace_back(Price::fromRaw(50001 * 1'000'000LL + ex * 100), Quantity::fromRaw(5 * 1'000'000LL));
    matrix.onBookUpdate(ev);
  }

  for (auto _ : state)
  {
    auto bid = matrix.bestBid(1);
    benchmark::DoNotOptimize(bid);
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_CompositeBookMatrix_BestAsk(benchmark::State& state)
{
  const size_t numExchanges = state.range(0);
  CompositeBookMatrix<8> matrix;

  std::pmr::monotonic_buffer_resource pool(1024 * 1024);
  BookUpdateEvent ev(&pool);

  for (size_t ex = 0; ex < numExchanges; ++ex)
  {
    ev.update.symbol = 1;
    ev.sourceExchange = static_cast<ExchangeId>(ex);
    ev.update.bids.clear();
    ev.update.asks.clear();
    ev.update.bids.emplace_back(Price::fromRaw(50000 * 1'000'000LL - ex * 100), Quantity::fromRaw(10 * 1'000'000LL));
    ev.update.asks.emplace_back(Price::fromRaw(50001 * 1'000'000LL - ex * 100), Quantity::fromRaw(5 * 1'000'000LL));
    matrix.onBookUpdate(ev);
  }

  for (auto _ : state)
  {
    auto ask = matrix.bestAsk(1);
    benchmark::DoNotOptimize(ask);
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_CompositeBookMatrix_ArbitrageCheck(benchmark::State& state)
{
  const size_t numExchanges = state.range(0);
  CompositeBookMatrix<8> matrix;

  std::pmr::monotonic_buffer_resource pool(1024 * 1024);
  BookUpdateEvent ev(&pool);

  for (size_t ex = 0; ex < numExchanges; ++ex)
  {
    ev.update.symbol = 1;
    ev.sourceExchange = static_cast<ExchangeId>(ex);
    ev.update.bids.clear();
    ev.update.asks.clear();
    ev.update.bids.emplace_back(Price::fromRaw(50000 * 1'000'000LL + ex * 100), Quantity::fromRaw(10 * 1'000'000LL));
    ev.update.asks.emplace_back(Price::fromRaw(50001 * 1'000'000LL - ex * 100), Quantity::fromRaw(5 * 1'000'000LL));
    matrix.onBookUpdate(ev);
  }

  for (auto _ : state)
  {
    bool arb = matrix.hasArbitrageOpportunity(1);
    benchmark::DoNotOptimize(arb);
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_CompositeBookMatrix_Update(benchmark::State& state)
{
  CompositeBookMatrix<8> matrix;

  std::pmr::monotonic_buffer_resource pool(1024 * 1024);
  BookUpdateEvent ev(&pool);
  ev.update.symbol = 1;
  ev.sourceExchange = 0;
  ev.update.bids.emplace_back(Price::fromRaw(50000 * 1'000'000LL), Quantity::fromRaw(10 * 1'000'000LL));
  ev.update.asks.emplace_back(Price::fromRaw(50001 * 1'000'000LL), Quantity::fromRaw(5 * 1'000'000LL));

  int64_t i = 0;
  for (auto _ : state)
  {
    ev.update.bids[0].price = Price::fromRaw(50000 * 1'000'000LL + (i++ % 100));
    matrix.onBookUpdate(ev);
  }

  state.SetItemsProcessed(state.iterations());
}

// ============================================================================
// AggregatedPositionTracker Benchmarks
// ============================================================================

static void BM_PositionTracker_SingleExchange(benchmark::State& state)
{
  AggregatedPositionTracker<8> tracker;

  // Pre-populate
  tracker.onFill(0, 1, Quantity::fromDouble(100), Price::fromDouble(50000));

  for (auto _ : state)
  {
    auto pos = tracker.position(0, 1);
    benchmark::DoNotOptimize(pos);
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_PositionTracker_TotalPosition(benchmark::State& state)
{
  const size_t numExchanges = state.range(0);
  AggregatedPositionTracker<8> tracker;

  // Pre-populate positions on multiple exchanges
  for (size_t ex = 0; ex < numExchanges; ++ex)
  {
    tracker.onFill(static_cast<ExchangeId>(ex), 1, Quantity::fromDouble(100), Price::fromDouble(50000));
  }

  for (auto _ : state)
  {
    auto total = tracker.totalPosition(1);
    benchmark::DoNotOptimize(total);
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_PositionTracker_OnFill(benchmark::State& state)
{
  AggregatedPositionTracker<8> tracker;

  int64_t i = 0;
  for (auto _ : state)
  {
    // Alternate buy/sell to avoid position growing too large
    Quantity qty = ((i++ % 2) == 0) ? Quantity::fromDouble(100) : Quantity::fromDouble(-100);
    tracker.onFill(0, 1, qty, Price::fromDouble(50000));
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_PositionTracker_UnrealizedPnL(benchmark::State& state)
{
  const size_t numExchanges = state.range(0);
  AggregatedPositionTracker<8> tracker;

  for (size_t ex = 0; ex < numExchanges; ++ex)
  {
    tracker.onFill(static_cast<ExchangeId>(ex), 1, Quantity::fromDouble(100), Price::fromDouble(50000));
  }

  for (auto _ : state)
  {
    Volume pnl = tracker.unrealizedPnl(1, Price::fromDouble(51000));
    benchmark::DoNotOptimize(pnl);
  }

  state.SetItemsProcessed(state.iterations());
}

// ============================================================================
// ExchangeClockSync Benchmarks
// ============================================================================

static void BM_ClockSync_RecordSample(benchmark::State& state)
{
  ExchangeClockSync<8> sync;

  int64_t i = 0;
  for (auto _ : state)
  {
    sync.recordSample(0, i * 1000, i * 1000 + 100, i * 1000 + 200);
    ++i;
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_ClockSync_ToLocalTime(benchmark::State& state)
{
  ExchangeClockSync<8> sync;

  // Pre-sync
  for (int i = 0; i < 10; ++i)
  {
    sync.recordSample(0, i * 1000, i * 1000 + 100, i * 1000 + 200);
  }

  int64_t exchangeTime = 1'000'000'000LL;
  for (auto _ : state)
  {
    int64_t localTime = sync.toLocalTimeNs(0, exchangeTime);
    benchmark::DoNotOptimize(localTime);
    ++exchangeTime;
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_ClockSync_Estimate(benchmark::State& state)
{
  ExchangeClockSync<8> sync;

  // Pre-sync
  for (int i = 0; i < 10; ++i)
  {
    sync.recordSample(0, i * 1000, i * 1000 + 100, i * 1000 + 200);
  }

  for (auto _ : state)
  {
    auto est = sync.estimate(0);
    benchmark::DoNotOptimize(est);
  }

  state.SetItemsProcessed(state.iterations());
}

// ============================================================================
// Thread-safety overhead benchmarks (single-threaded baseline)
// ============================================================================

static void BM_AtomicLoad_Baseline(benchmark::State& state)
{
  std::atomic<int64_t> value{42};

  for (auto _ : state)
  {
    int64_t v = value.load(std::memory_order_acquire);
    benchmark::DoNotOptimize(v);
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_AtomicStore_Baseline(benchmark::State& state)
{
  std::atomic<int64_t> value{0};

  int64_t i = 0;
  for (auto _ : state)
  {
    value.store(i++, std::memory_order_release);
  }

  state.SetItemsProcessed(state.iterations());
}

// Register benchmarks
BENCHMARK(BM_CompositeBookMatrix_BestBid)->Arg(1)->Arg(2)->Arg(4)->Arg(8);
BENCHMARK(BM_CompositeBookMatrix_BestAsk)->Arg(1)->Arg(2)->Arg(4)->Arg(8);
BENCHMARK(BM_CompositeBookMatrix_ArbitrageCheck)->Arg(2)->Arg(4)->Arg(8);
BENCHMARK(BM_CompositeBookMatrix_Update);

BENCHMARK(BM_PositionTracker_SingleExchange);
BENCHMARK(BM_PositionTracker_TotalPosition)->Arg(1)->Arg(2)->Arg(4)->Arg(8);
BENCHMARK(BM_PositionTracker_OnFill);
BENCHMARK(BM_PositionTracker_UnrealizedPnL)->Arg(1)->Arg(4)->Arg(8);

BENCHMARK(BM_ClockSync_RecordSample);
BENCHMARK(BM_ClockSync_ToLocalTime);
BENCHMARK(BM_ClockSync_Estimate);

BENCHMARK(BM_AtomicLoad_Baseline);
BENCHMARK(BM_AtomicStore_Baseline);

BENCHMARK_MAIN();
