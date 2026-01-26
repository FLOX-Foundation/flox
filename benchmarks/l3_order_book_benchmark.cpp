/*
  * Flox Engine
  * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
  *
  * Copyright (c) 2025 FLOX Foundation
  * Licensed under the MIT License. See LICENSE file in the project root for full
  * license information.
  */

#include "flox/book/l3/l3_order_book.h"
#include "flox/common.h"

#include <benchmark/benchmark.h>

using namespace flox;

static void BM_L3_AddOrder_NewLevel(benchmark::State& state)
{
  constexpr size_t N = 8192;
  L3OrderBook<N> book;

  const Quantity qty = Quantity::fromDouble(1.0);
  const Price px = Price::fromDouble(100.0);
  const OrderId id{1};

  for (auto _ : state)
  {
    // Add to non-existing level (forces level creation)
    benchmark::DoNotOptimize(book.addOrder(id, px, qty, Side::BUY));

    state.PauseTiming();
    // Remove immediately so next iteration is identical
    book.removeOrder(id);
    state.ResumeTiming();
  }
}
BENCHMARK(BM_L3_AddOrder_NewLevel)->Unit(benchmark::kNanosecond);

static void BM_L3_RemoveOrder_DeleteLevel(benchmark::State& state)
{
  constexpr size_t N = 8192;
  L3OrderBook<N> book;

  const Quantity qty = Quantity::fromDouble(1.0);
  const Price px = Price::fromDouble(100.0);
  const OrderId id{1};
  const Side side{Side::BUY};

  book.addOrder(id, px, qty, side);

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.removeOrder(id));

    state.PauseTiming();
    book.addOrder(id, px, qty, side);
    state.ResumeTiming();
  }
}
BENCHMARK(BM_L3_RemoveOrder_DeleteLevel)->Unit(benchmark::kNanosecond);

static void BM_L3_AddOrder_ExistingLevel(benchmark::State& state)
{
  constexpr size_t N = 8192;
  L3OrderBook<N> book;

  const Price px = Price::fromDouble(100.0);
  const Quantity qty = Quantity::fromDouble(1.0);

  // Create the price level once
  book.addOrder(OrderId{2}, px, qty, Side::BUY);

  const OrderId id{1};  // IMPORTANT: reuse same ID every iteration

  for (auto _ : state)
  {
    const auto st = book.addOrder(id, px, qty, Side::BUY);
    benchmark::DoNotOptimize(book);

    state.PauseTiming();
    book.removeOrder(id);  // restore invariant WITHOUT poisoning hash table
    state.ResumeTiming();
  }
}
BENCHMARK(BM_L3_AddOrder_ExistingLevel)->Unit(benchmark::kNanosecond);

static void BM_L3_RemoveOrder_KeepLevel(benchmark::State& state)
{
  constexpr size_t N = 8192;
  L3OrderBook<N> book;

  const Quantity qty = Quantity::fromDouble(1.0);
  const Price px = Price::fromDouble(100.0);
  const OrderId id{1};
  const Side side{Side::BUY};

  book.addOrder(id, px, qty, side);
  book.addOrder(OrderId{2}, px, qty, side);

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.removeOrder(id));

    state.PauseTiming();
    book.addOrder(id, px, qty, side);
    state.ResumeTiming();
  }
}
BENCHMARK(BM_L3_RemoveOrder_KeepLevel)->Unit(benchmark::kNanosecond);

static void BM_L3_ModifyOrder(benchmark::State& state)
{
  constexpr size_t N = 8192;
  L3OrderBook<N> book;

  const Price px = Price::fromDouble(100.0);
  const Quantity qty = Quantity::fromDouble(1.0);

  // Create the price level once
  book.addOrder(OrderId{2}, px, qty, Side::BUY);

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.modifyOrder(OrderId{2}, Quantity::fromDouble(qty.toDouble() + 2.0)));
  }
}
BENCHMARK(BM_L3_ModifyOrder)->Unit(benchmark::kNanosecond);

// This benchmark serves to illustrate what happens to addOrder latency when the book is treated like an immortal hash table.
/*
What is happening under the benchmark's hood:
    - monotonically increases order IDs
    - linear probing hash table
    - no table rebuild
    - no lifecycle boundaries
    - infinite hash table tombstone accumulation
Effect:
    - probe chains grow
    - cache locality dies
    - addOrder degrades to near full table scans
    - latency spikes
*/
static void BM_L3_AddOrder_TombstoneChurn(benchmark::State& state)
{
  constexpr size_t N = 8192;
  L3OrderBook<N> book;

  const Price px = Price::fromDouble(100.0);
  const Quantity qty = Quantity::fromDouble(1.0);

  book.addOrder(OrderId{1}, px, qty, Side::BUY);

  OrderId id{2};

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.addOrder(id, px, qty, Side::BUY));
    book.removeOrder(id);
    ++id;  // poison hash table intentionally
  }
}
BENCHMARK(BM_L3_AddOrder_TombstoneChurn)->Unit(benchmark::kMicrosecond);  // Note: microsecond scale due to near-full table scans

// This benchmark serves to model a more realistic L3 lifecycle and the associated performance.
/*
What is happening under the benchmark's hood:
    - bounded ID domain
    - bounded tombstone count
    - periodic book rebuild (session / snapshot / replay boundary)

Result:
    - probe chains stay short
    - hash stays hot in L1/L2
    - addOrder remains O(1) in practice
*/
static void BM_L3_AddOrder_TombstoneChurn_WithReset(benchmark::State& state)
{
  constexpr size_t N = 8192;
  constexpr size_t K = 10'000;  // arbitrary but explicit

  L3OrderBook<N> book;
  const Price px = Price::fromDouble(100.0);
  const Quantity qty = Quantity::fromDouble(1.0);

  book.addOrder(OrderId{1}, px, qty, Side::BUY);
  OrderId id{2};
  size_t since_reset = 0;

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.addOrder(id, px, qty, Side::BUY));
    book.removeOrder(id);
    ++id;

    if (++since_reset == K)
    {
      state.PauseTiming();
      auto ss = book.exportSnapshot();
      book.buildFromSnapshot(ss);
      since_reset = 0;
      state.ResumeTiming();
    }
  }
}

BENCHMARK(BM_L3_AddOrder_TombstoneChurn_WithReset)->Unit(benchmark::kNanosecond);

static void BM_L3_BestBid(benchmark::State& state)
{
  constexpr std::size_t N = 8192;
  L3OrderBook<N> book;
  Price px = Price::fromDouble(100.0);
  Quantity qty = Quantity::fromDouble(5.0);
  Side side = Side::BUY;

  for (std::uint64_t i = 0; i < 1000; ++i)
  {
    book.addOrder(OrderId{123 + i}, Price::fromDouble(px.toDouble() + 1.0), qty, side);
  }

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.bestBid());
  }
}
BENCHMARK(BM_L3_BestBid)->Unit(benchmark::kNanosecond);

static void BM_L3_BestAsk(benchmark::State& state)
{
  constexpr std::size_t N = 8192;
  L3OrderBook<N> book;
  Price px = Price::fromDouble(100.0);
  Quantity qty = Quantity::fromDouble(5.0);
  Side side = Side::SELL;

  for (std::uint64_t i = 0; i < 1000; ++i)
  {
    book.addOrder(OrderId{123 + i}, Price::fromDouble(px.toDouble() + 1.0), qty, side);
  }

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.bestAsk());
  }
}
BENCHMARK(BM_L3_BestAsk)->Unit(benchmark::kNanosecond);

static void BM_L3_BidAtPrice(benchmark::State& state)
{
  constexpr std::size_t N = 5000;
  L3OrderBook<N> book;
  Price px = Price::fromDouble(100.0);
  Quantity qty = Quantity::fromDouble(5.0);
  Side side = Side::BUY;

  for (std::uint64_t i = 0; i < 50; ++i)
  {
    book.addOrder(OrderId{1 + i}, px, qty, side);
  }

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.bidAtPrice(px));
  }
}
BENCHMARK(BM_L3_BidAtPrice)->Unit(benchmark::kNanosecond);

static void BM_L3_AskAtPrice(benchmark::State& state)
{
  constexpr std::size_t N = 5000;
  L3OrderBook<N> book;
  Price px = Price::fromDouble(100.0);
  Quantity qty = Quantity::fromDouble(5.0);
  Side side = Side::SELL;

  for (std::uint64_t i = 0; i < 50; ++i)
  {
    book.addOrder(OrderId{1 + i}, px, qty, side);
  }

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.askAtPrice(px));
  }
}
BENCHMARK(BM_L3_AskAtPrice)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
