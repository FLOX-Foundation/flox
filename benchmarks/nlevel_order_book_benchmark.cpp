/*
  * Flox Engine
  * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
  *
  * Copyright (c) 2025 FLOX Foundation
  * Licensed under the MIT License. See LICENSE file in the project root for full
  * license information.
  */

#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"
#include "flox/common.h"
#include "flox/util/memory/pool.h"

#include <benchmark/benchmark.h>
#include <random>

using namespace flox;

using BookUpdatePool = pool::Pool<BookUpdateEvent, 63>;

static void BM_ApplyBookUpdate(benchmark::State& state)
{
  NLevelOrderBook<> book{Price::fromDouble(0.1)};
  BookUpdatePool pool;

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(19900.0, 20100.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  for (auto _ : state)
  {
    auto opt = pool.acquire();
    assert(opt);

    auto& update = *opt;
    update->update.type = BookUpdateType::DELTA;
    update->update.timestamp = std::chrono::steady_clock::now();

    update->update.bids.clear();
    update->update.asks.clear();
    update->update.bids.reserve(10000);
    update->update.asks.reserve(10000);

    for (int i = 0; i < 10000; ++i)
    {
      Price price = Price::fromDouble(priceDist(rng));
      Quantity qty = Quantity::fromDouble(qtyDist(rng));
      update->update.bids.push_back({price, qty});
      update->update.asks.push_back({Price::fromDouble(price.toDouble() + 10.0), qty});
    }

    book.applyBookUpdate(*update);
  }
}
BENCHMARK(BM_ApplyBookUpdate)->Unit(benchmark::kMicrosecond);

static void BM_BestBid(benchmark::State& state)
{
  NLevelOrderBook<100000> book{Price::fromDouble(0.1)};
  BookUpdatePool pool;

  auto opt = pool.acquire();
  assert(opt);

  auto& update = *opt;
  update->update.type = BookUpdateType::SNAPSHOT;
  update->update.bids.reserve(100000);
  update->update.asks.clear();

  for (int i = 0; i < 100000; ++i)
  {
    Price price =
        Price::fromRaw(Price::fromDouble(20000.0).raw() - i * Price::fromDouble(0.1).raw());
    update->update.bids.push_back({price, Quantity::fromDouble(1.0)});
  }

  book.applyBookUpdate(*update);

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.bestBid());
  }
}
BENCHMARK(BM_BestBid)->Unit(benchmark::kNanosecond);

static void BM_BestAsk(benchmark::State& state)
{
  NLevelOrderBook<100000> book{Price::fromDouble(0.1)};
  BookUpdatePool pool;

  auto opt = pool.acquire();
  if (!opt)
  {
    return;
  }

  auto& update = *opt;
  update->update.type = BookUpdateType::SNAPSHOT;
  update->update.asks.reserve(100000);
  update->update.bids.clear();

  for (int i = 0; i < 100000; ++i)
  {
    Price price =
        Price::fromRaw(Price::fromDouble(20000.0).raw() + i * Price::fromDouble(0.1).raw());
    update->update.asks.push_back({price, Quantity::fromDouble(1.0)});
  }

  book.applyBookUpdate(*update);

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(book.bestAsk());
  }
}
BENCHMARK(BM_BestAsk)->Unit(benchmark::kNanosecond);

static void BM_ConsumeAsks_Dense(benchmark::State& state)
{
  constexpr int kLevels = 100000;
  NLevelOrderBook<kLevels> book{Price::fromDouble(0.1)};
  BookUpdatePool pool;

  auto opt = pool.acquire();
  assert(opt);
  auto& up = *opt;

  up->update.type = BookUpdateType::SNAPSHOT;
  up->update.asks.reserve(kLevels);
  up->update.bids.clear();

  const auto p0 = Price::fromDouble(20000.0);
  const auto ts = Price::fromDouble(0.1).raw();

  for (int i = 0; i < kLevels; ++i)
  {
    Price px = Price::fromRaw(p0.raw() + int64_t(i) * ts);
    double qd = 0.5 + (i % 10) * 0.15;
    up->update.asks.emplace_back(px, Quantity::fromDouble(qd));
  }

  book.applyBookUpdate(*up);

  const double needQtyBase = 250.0;
  for (auto _ : state)
  {
    auto res = book.consumeAsks(needQtyBase);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_ConsumeAsks_Dense)->Unit(benchmark::kMicrosecond);

static void BM_ConsumeBids_Dense(benchmark::State& state)
{
  constexpr int kLevels = 100000;
  NLevelOrderBook<kLevels> book{Price::fromDouble(0.1)};
  BookUpdatePool pool;

  auto opt = pool.acquire();
  assert(opt);
  auto& up = *opt;

  up->update.type = BookUpdateType::SNAPSHOT;
  up->update.bids.reserve(kLevels);
  up->update.asks.clear();

  const auto p0 = Price::fromDouble(20000.0);
  const auto ts = Price::fromDouble(0.1).raw();

  for (int i = 0; i < kLevels; ++i)
  {
    Price px = Price::fromRaw(p0.raw() - int64_t(i) * ts);
    double qd = 0.5 + (i % 10) * 0.15;
    up->update.bids.emplace_back(px, Quantity::fromDouble(qd));
  }

  book.applyBookUpdate(*up);

  const double needQtyBase = 250.0;
  for (auto _ : state)
  {
    auto res = book.consumeBids(needQtyBase);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_ConsumeBids_Dense)->Unit(benchmark::kMicrosecond);

static void BM_ConsumeAsks_Sparse(benchmark::State& state)
{
  constexpr int kLevels = 100000;
  NLevelOrderBook<kLevels> book{Price::fromDouble(0.1)};
  BookUpdatePool pool;

  auto opt = pool.acquire();
  assert(opt);
  auto& up = *opt;

  up->update.type = BookUpdateType::SNAPSHOT;
  up->update.asks.reserve(kLevels);
  up->update.bids.clear();

  const auto p0 = Price::fromDouble(20000.0);
  const auto ts = Price::fromDouble(0.1).raw();

  for (int i = 0; i < kLevels; ++i)
  {
    Price px = Price::fromRaw(p0.raw() + int64_t(i) * ts);
    double qd = (i % 4 == 0) ? (0.5 + (i % 10) * 0.15) : 0.0;
    up->update.asks.emplace_back(px, Quantity::fromDouble(qd));
  }

  book.applyBookUpdate(*up);

  const double needQtyBase = 250.0;
  for (auto _ : state)
  {
    auto res = book.consumeAsks(needQtyBase);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_ConsumeAsks_Sparse)->Unit(benchmark::kMicrosecond);

static void BM_ConsumeBids_Sparse(benchmark::State& state)
{
  constexpr int kLevels = 100000;
  NLevelOrderBook<kLevels> book{Price::fromDouble(0.1)};
  BookUpdatePool pool;

  auto opt = pool.acquire();
  assert(opt);
  auto& up = *opt;

  up->update.type = BookUpdateType::SNAPSHOT;
  up->update.bids.reserve(kLevels);
  up->update.asks.clear();

  const auto p0 = Price::fromDouble(20000.0);
  const auto ts = Price::fromDouble(0.1).raw();

  for (int i = 0; i < kLevels; ++i)
  {
    Price px = Price::fromRaw(p0.raw() - int64_t(i) * ts);
    double qd = (i % 4 == 0) ? (0.5 + (i % 10) * 0.15) : 0.0;
    up->update.bids.emplace_back(px, Quantity::fromDouble(qd));
  }

  book.applyBookUpdate(*up);

  const double needQtyBase = 250.0;
  for (auto _ : state)
  {
    auto res = book.consumeBids(needQtyBase);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_ConsumeBids_Sparse)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
