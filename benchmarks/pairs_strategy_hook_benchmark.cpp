/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// W28-T046: measures the wall-clock cost of a real native strategy's hook
// body, end to end through Strategy::onBookUpdate() (the public entry point
// that takes the per-symbol lock, applies the book update, and dispatches
// to the protected onSymbolBook() override) -- i.e. exactly the span the
// per-symbol lock is held for today. demo::PairsStrategy is used because it
// is the one real, shipped native strategy in the tree: a rolling z-score
// over a 100-sample spread history, computed on every book update once
// warmed up, which is representative of an ordinary native strategy's
// per-event cost (not a synthetic spin).

#include "demo/pairs_strategy.h"
#include "flox/engine/symbol_registry.h"

#include <benchmark/benchmark.h>
#include <memory_resource>
#include <random>

using namespace flox;

namespace
{
void fillSnapshot(BookUpdateEvent& ev, SymbolId sym, double bid, double ask)
{
  ev.update.bids.clear();
  ev.update.asks.clear();
  ev.update.symbol = sym;
  ev.update.type = BookUpdateType::SNAPSHOT;
  ev.update.bids.emplace_back(Price::fromDouble(bid), Quantity::fromDouble(1.0));
  ev.update.asks.emplace_back(Price::fromDouble(ask), Quantity::fromDouble(1.0));
}

// Exposes the protected onSymbolBook() hook body directly, with the
// surrounding lock / applyBookUpdate() / refreshPosition() machinery
// (Strategy::onBookUpdate()) excluded -- isolating exactly the span T046
// is asking about (the user hook itself), separate from the book-apply
// cost that stays inside the lock either way narrowing goes.
class BenchPairsStrategy : public demo::PairsStrategy
{
 public:
  using demo::PairsStrategy::ctx;
  using demo::PairsStrategy::PairsStrategy;
  void callHook(const BookUpdateEvent& ev) { onSymbolBook(ctx(cfgLeg1()), ev); }
  SymbolId cfgLeg1() const { return ctx().symbolId; }
};

struct Fixture
{
  SymbolRegistry registry;
  SymbolId leg1;
  SymbolId leg2;
  demo::PairsStrategy strat;

  explicit Fixture(size_t lookbackPeriod)
      : leg1(registerLeg(registry, "LEG1")),
        leg2(registerLeg(registry, "LEG2")),
        strat(1, makeConfig(leg1, leg2, lookbackPeriod), registry)
  {
    strat.start();
  }

  static SymbolId registerLeg(SymbolRegistry& reg, const char* symbol)
  {
    SymbolInfo info;
    info.exchange = "BENCH";
    info.symbol = symbol;
    info.tickSize = Price::fromDouble(0.01);
    return reg.registerSymbol(info);
  }

  static demo::PairsConfig makeConfig(SymbolId leg1, SymbolId leg2, size_t lookbackPeriod)
  {
    demo::PairsConfig cfg;
    cfg.leg1 = leg1;
    cfg.leg2 = leg2;
    cfg.lookbackPeriod = lookbackPeriod;
    return cfg;
  }
};
}  // namespace

// Steady state: spread history already at lookbackPeriod, so every call
// exercises the full z-score computation (the representative per-event
// cost, not the empty-history fast path of the first 100 calls).
static void BM_PairsStrategyOnBookUpdate_SteadyState(benchmark::State& state)
{
  Fixture f(100);
  auto* res = std::pmr::new_delete_resource();

  // leg2's book only needs to be valid once; PairsStrategy never updates it
  // in this benchmark, matching the common case of a hedge leg that quotes
  // far less often than the primary leg.
  BookUpdateEvent leg2Ev(res);
  fillSnapshot(leg2Ev, f.leg2, 100.0, 100.02);
  f.strat.onBookUpdate(leg2Ev);

  std::mt19937 rng(42);
  std::uniform_real_distribution<double> jitter(-0.05, 0.05);

  BookUpdateEvent ev(res);

  // Warm the rolling window to lookbackPeriod before timing starts, so the
  // measured loop always takes the full computeZScore() path.
  for (size_t i = 0; i < 100; ++i)
  {
    double mid = 100.0 + jitter(rng);
    fillSnapshot(ev, f.leg1, mid, mid + 0.02);
    f.strat.onBookUpdate(ev);
  }

  for (auto _ : state)
  {
    double mid = 100.0 + jitter(rng);
    fillSnapshot(ev, f.leg1, mid, mid + 0.02);
    benchmark::DoNotOptimize(ev);
    f.strat.onBookUpdate(ev);
  }
}
BENCHMARK(BM_PairsStrategyOnBookUpdate_SteadyState);

// Cold state: spread history below lookbackPeriod, so onSymbolBook returns
// right after updateSpreadHistory() -- the cheap end of the same real
// strategy's hook, for contrast against the steady-state number above.
static void BM_PairsStrategyOnBookUpdate_ColdState(benchmark::State& state)
{
  Fixture f(100);
  auto* res = std::pmr::new_delete_resource();

  BookUpdateEvent leg2Ev(res);
  fillSnapshot(leg2Ev, f.leg2, 100.0, 100.02);
  f.strat.onBookUpdate(leg2Ev);

  std::mt19937 rng(42);
  std::uniform_real_distribution<double> jitter(-0.05, 0.05);

  BookUpdateEvent ev(res);
  for (auto _ : state)
  {
    double mid = 100.0 + jitter(rng);
    fillSnapshot(ev, f.leg1, mid, mid + 0.02);
    benchmark::DoNotOptimize(ev);
    f.strat.onBookUpdate(ev);
  }
}
BENCHMARK(BM_PairsStrategyOnBookUpdate_ColdState);

// The isolated hook body: onSymbolBook() alone, with the lock /
// applyBookUpdate() / refreshPosition() machinery that always runs inside
// Strategy's per-symbol lock excluded. This is T046's actual subject --
// "how long does a typical hook run for" -- separate from the book-write
// cost measured above, which narrowing the lock would not remove either
// way.
static void BM_PairsStrategyHookOnly_SteadyState(benchmark::State& state)
{
  SymbolRegistry registry;
  SymbolInfo leg1Info;
  leg1Info.exchange = "BENCH";
  leg1Info.symbol = "LEG1";
  leg1Info.tickSize = Price::fromDouble(0.01);
  SymbolId leg1 = registry.registerSymbol(leg1Info);

  SymbolInfo leg2Info;
  leg2Info.exchange = "BENCH";
  leg2Info.symbol = "LEG2";
  leg2Info.tickSize = Price::fromDouble(0.01);
  SymbolId leg2 = registry.registerSymbol(leg2Info);

  demo::PairsConfig cfg;
  cfg.leg1 = leg1;
  cfg.leg2 = leg2;
  cfg.lookbackPeriod = 100;
  BenchPairsStrategy strat(1, cfg, registry);
  strat.start();

  auto* res = std::pmr::new_delete_resource();

  BookUpdateEvent leg2Ev(res);
  fillSnapshot(leg2Ev, leg2, 100.0, 100.02);
  strat.onBookUpdate(leg2Ev);

  std::mt19937 rng(42);
  std::uniform_real_distribution<double> jitter(-0.05, 0.05);

  BookUpdateEvent ev(res);
  for (size_t i = 0; i < 100; ++i)
  {
    double mid = 100.0 + jitter(rng);
    fillSnapshot(ev, leg1, mid, mid + 0.02);
    strat.onBookUpdate(ev);
  }

  // ev's content does not matter to callHook() -- onSymbolBook() reads the
  // already-updated ctx(), not the event -- so it is reused unmodified;
  // the deque push/pop and the full sum/sumSq loop over 100 samples still
  // run on every call.
  for (auto _ : state)
  {
    strat.callHook(ev);
  }
}
BENCHMARK(BM_PairsStrategyHookOnly_SteadyState);

BENCHMARK_MAIN();
