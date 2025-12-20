/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/engine/symbol_registry.h"
#include "flox/strategy/strategy.h"
#include "flox/strategy/symbol_state_map.h"

#include <benchmark/benchmark.h>
#include <random>
#include <unordered_map>

using namespace flox;

namespace
{
void populateRegistry(SymbolRegistry& registry, size_t numSymbols)
{
  for (size_t i = 0; i < numSymbols; ++i)
  {
    SymbolInfo info;
    info.exchange = "BENCH";
    info.symbol = "SYM" + std::to_string(i);
    info.tickSize = Price::fromDouble(0.01);
    registry.registerSymbol(info);
  }
}
}  // namespace

struct BenchState
{
  int64_t value{0};
  double price{0.0};
  int counter{0};
};

static void BM_SymbolStateMapAccess(benchmark::State& state)
{
  SymbolStateMap<BenchState> map;
  const size_t numSymbols = state.range(0);

  for (size_t i = 0; i < numSymbols; ++i)
  {
    map[i].value = static_cast<int64_t>(i);
  }

  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, numSymbols - 1);

  for (auto _ : state)
  {
    SymbolId sym = static_cast<SymbolId>(dist(rng));
    benchmark::DoNotOptimize(map[sym].value);
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_UnorderedMapAccess(benchmark::State& state)
{
  std::unordered_map<SymbolId, BenchState> map;
  const size_t numSymbols = state.range(0);

  for (size_t i = 0; i < numSymbols; ++i)
  {
    map[i].value = static_cast<int64_t>(i);
  }

  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, numSymbols - 1);

  for (auto _ : state)
  {
    SymbolId sym = static_cast<SymbolId>(dist(rng));
    benchmark::DoNotOptimize(map[sym].value);
  }

  state.SetItemsProcessed(state.iterations());
}

class BenchStrategy : public Strategy
{
 public:
  BenchStrategy(std::vector<SymbolId> syms, const SymbolRegistry& registry)
      : Strategy(1, std::move(syms), registry)
  {
  }

  void start() override {}
  void stop() override {}

 protected:
  void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override { c.lastTradePrice = ev.trade.price; }

  void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override {}
};

static void BM_MultiSymbolStrategyDispatch(benchmark::State& state)
{
  const size_t numSymbols = state.range(0);

  std::vector<SymbolId> symbols;
  for (size_t i = 0; i < numSymbols; ++i)
  {
    symbols.push_back(static_cast<SymbolId>(i + 1));  // SymbolIds are 1-based
  }

  SymbolRegistry registry;
  populateRegistry(registry, numSymbols);
  BenchStrategy strategy(symbols, registry);

  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, numSymbols - 1);

  TradeEvent ev;
  ev.trade.price = Price::fromDouble(100.0);
  ev.trade.quantity = Quantity::fromDouble(1.0);

  for (auto _ : state)
  {
    ev.trade.symbol = static_cast<SymbolId>(dist(rng));
    strategy.onTrade(ev);
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_SymbolStateMapForEach(benchmark::State& state)
{
  SymbolStateMap<BenchState> map;
  const size_t numSymbols = state.range(0);

  for (size_t i = 0; i < numSymbols; ++i)
  {
    map[i].value = static_cast<int64_t>(i);
  }

  for (auto _ : state)
  {
    int64_t sum = 0;
    map.forEach([&sum](SymbolId, const BenchState& s)
                { sum += s.value; });
    benchmark::DoNotOptimize(sum);
  }

  state.SetItemsProcessed(state.iterations() * numSymbols);
}

BENCHMARK(BM_SymbolStateMapAccess)->Arg(10)->Arg(50)->Arg(200);
BENCHMARK(BM_UnorderedMapAccess)->Arg(10)->Arg(50)->Arg(200);
BENCHMARK(BM_MultiSymbolStrategyDispatch)->Arg(10)->Arg(50)->Arg(200);
BENCHMARK(BM_SymbolStateMapForEach)->Arg(10)->Arg(50)->Arg(200);

BENCHMARK_MAIN();
