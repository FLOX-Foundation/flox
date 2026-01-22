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

static void BM_L3_AddOrder(benchmark::State& state)
{
  constexpr std::size_t N = 8192;
  L3OrderBook<N> book;
  OrderId id{123};
  Price price = Price::fromDouble(100.0);
  Quantity qty = Quantity::fromDouble(5.0);
  for (auto _ : state)
  {
    state.PauseTiming();
    book.removeOrder(id);
    state.ResumeTiming();

    benchmark::DoNotOptimize(book.addOrder(id, price, qty, Side::SELL));
  }
}

BENCHMARK(BM_L3_AddOrder)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
