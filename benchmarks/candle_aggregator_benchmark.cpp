/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/aggregator/bus/candle_bus.h"
#include "flox/aggregator/candle_aggregator.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"

#include <benchmark/benchmark.h>
#include <random>

using namespace flox;

static void BM_CandleAggregator_OnTrade(benchmark::State& state)
{
  constexpr SymbolId SYMBOL = 42;
  constexpr std::chrono::seconds INTERVAL(60);

  CandleBus bus;
  CandleAggregator aggregator(INTERVAL, &bus);
  bus.start();
  aggregator.start();

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(100.0, 110.0);
  std::uniform_real_distribution<> qtyDist(1.0, 5.0);

  int64_t baseTs = 0;

  for (auto _ : state)
  {
    TradeEvent event;

    event.trade.symbol = SYMBOL;
    event.trade.price = Price::fromDouble(priceDist(rng));
    event.trade.quantity = Quantity::fromDouble(qtyDist(rng));
    event.trade.isBuy = true;
    event.trade.timestamp =
        std::chrono::steady_clock::time_point(std::chrono::seconds(baseTs++));

    aggregator.onTrade(event);
  }

  aggregator.stop();
  bus.stop();
}

// Registers the benchmark with 1M iterations
BENCHMARK(BM_CandleAggregator_OnTrade)->Iterations(1'000'000);

BENCHMARK_MAIN();