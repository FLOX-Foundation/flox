/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <benchmark/benchmark.h>

#include "flox/execution/multi_execution_listener.h"
#include "flox/position/multi_mode_position_tracker.h"
#include "flox/position/position_group.h"
#include "flox/position/position_reconciler.h"

using namespace flox;

namespace
{

Order makeOrder(OrderId id, SymbolId sym, Side side, double price, double qty,
                bool reduceOnly = false, uint16_t tag = 0)
{
  Order o{};
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  o.flags.reduceOnly = reduceOnly ? 1 : 0;
  o.orderTag = tag;
  return o;
}

struct OrderSequence
{
  std::vector<Order> orders;

  OrderSequence(size_t n, size_t numSymbols = 4)
  {
    orders.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
      SymbolId sym = static_cast<SymbolId>(100 + (i % numSymbols));
      Side side = (i % 3 == 0) ? Side::SELL : Side::BUY;
      double price = 100.0 + (i % 50);
      double qty = 0.1 + (i % 10) * 0.1;
      orders.push_back(makeOrder(i + 1, sym, side, price, qty));
    }
  }
};

static void BM_NetMode_Fill(benchmark::State& state)
{
  OrderSequence seq(10000);
  for (auto _ : state)
  {
    MultiModePositionTracker tracker{1, PositionAggregationMode::NET, CostBasisMethod::FIFO};
    for (const auto& order : seq.orders)
    {
      tracker.onOrderFilled(order);
    }
    benchmark::DoNotOptimize(tracker.getPosition(100));
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_NetMode_Fill);

static void BM_NetMode_FillSingleSymbol(benchmark::State& state)
{
  OrderSequence seq(10000, 1);
  for (auto _ : state)
  {
    MultiModePositionTracker tracker{1, PositionAggregationMode::NET, CostBasisMethod::FIFO};
    for (const auto& order : seq.orders)
    {
      tracker.onOrderFilled(order);
    }
    benchmark::DoNotOptimize(tracker.getPosition(100));
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_NetMode_FillSingleSymbol);

static void BM_NetMode_LIFO(benchmark::State& state)
{
  OrderSequence seq(10000);
  for (auto _ : state)
  {
    MultiModePositionTracker tracker{1, PositionAggregationMode::NET, CostBasisMethod::LIFO};
    for (const auto& order : seq.orders)
    {
      tracker.onOrderFilled(order);
    }
    benchmark::DoNotOptimize(tracker.getPosition(100));
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_NetMode_LIFO);

static void BM_NetMode_AVERAGE(benchmark::State& state)
{
  OrderSequence seq(10000);
  for (auto _ : state)
  {
    MultiModePositionTracker tracker{1, PositionAggregationMode::NET, CostBasisMethod::AVERAGE};
    for (const auto& order : seq.orders)
    {
      tracker.onOrderFilled(order);
    }
    benchmark::DoNotOptimize(tracker.getPosition(100));
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_NetMode_AVERAGE);

// Single fill latency
static void BM_NetMode_SingleFill(benchmark::State& state)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET, CostBasisMethod::FIFO};
  Order order = makeOrder(1, 100, Side::BUY, 100.0, 1.0);
  uint64_t id = 1;
  for (auto _ : state)
  {
    order.id = ++id;
    order.side = (id % 2) ? Side::BUY : Side::SELL;
    tracker.onOrderFilled(order);
    benchmark::DoNotOptimize(tracker.getPosition(100));
  }
}
BENCHMARK(BM_NetMode_SingleFill);

static void BM_PerSideMode_Fill(benchmark::State& state)
{
  OrderSequence seq(10000);
  for (auto _ : state)
  {
    MultiModePositionTracker tracker{1, PositionAggregationMode::PER_SIDE, CostBasisMethod::FIFO};
    for (const auto& order : seq.orders)
    {
      tracker.onOrderFilled(order);
    }
    benchmark::DoNotOptimize(tracker.getLongPosition(100));
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_PerSideMode_Fill);

static void BM_PerSideMode_FillAndClose(benchmark::State& state)
{
  for (auto _ : state)
  {
    MultiModePositionTracker tracker{1, PositionAggregationMode::PER_SIDE, CostBasisMethod::FIFO};
    for (int i = 0; i < 5000; ++i)
    {
      tracker.onOrderFilled(makeOrder(i * 2 + 1, 100, Side::BUY, 100.0 + i, 1.0));
      tracker.onOrderFilled(makeOrder(i * 2 + 2, 100, Side::SELL, 110.0 + i, 1.0, true));
    }
    benchmark::DoNotOptimize(tracker.getRealizedPnl(100));
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_PerSideMode_FillAndClose);

static void BM_GroupedMode_Fill(benchmark::State& state)
{
  OrderSequence seq(10000);
  for (auto _ : state)
  {
    MultiModePositionTracker tracker{1, PositionAggregationMode::GROUPED, CostBasisMethod::FIFO};
    for (const auto& order : seq.orders)
    {
      tracker.onOrderFilled(order);
    }
    benchmark::DoNotOptimize(tracker.getPosition(100));
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_GroupedMode_Fill);

static void BM_GroupedMode_FillWithTags(benchmark::State& state)
{
  std::vector<Order> orders;
  orders.reserve(10000);
  for (int i = 0; i < 10000; ++i)
  {
    uint16_t tag = static_cast<uint16_t>(1 + (i % 100));
    orders.push_back(makeOrder(i + 1, 100, Side::BUY, 100.0 + i, 0.5, false, tag));
  }

  for (auto _ : state)
  {
    MultiModePositionTracker tracker{1, PositionAggregationMode::GROUPED, CostBasisMethod::FIFO};
    for (const auto& order : orders)
    {
      tracker.onOrderFilled(order);
    }
    benchmark::DoNotOptimize(tracker.getPosition(100));
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_GroupedMode_FillWithTags);

static void BM_GroupedMode_NetPositionQuery(benchmark::State& state)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::GROUPED, CostBasisMethod::FIFO};
  for (int i = 0; i < 10000; ++i)
  {
    tracker.onOrderFilled(makeOrder(i + 1, static_cast<SymbolId>(100 + i % 4),
                                    (i % 2) ? Side::BUY : Side::SELL, 100.0, 1.0));
  }

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(tracker.getPosition(100));
    benchmark::DoNotOptimize(tracker.getPosition(101));
    benchmark::DoNotOptimize(tracker.getPosition(102));
    benchmark::DoNotOptimize(tracker.getPosition(103));
  }
}
BENCHMARK(BM_GroupedMode_NetPositionQuery);

static void BM_GroupTracker_OpenClose(benchmark::State& state)
{
  for (auto _ : state)
  {
    PositionGroupTracker gt;
    for (int i = 0; i < 5000; ++i)
    {
      PositionId pid = gt.openPosition(i + 1, 100, Side::BUY,
                                       Price::fromDouble(100.0 + i), Quantity::fromDouble(1.0));
      gt.closePosition(pid, Price::fromDouble(110.0 + i));
    }
    benchmark::DoNotOptimize(gt.totalRealizedPnl());
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_GroupTracker_OpenClose);

static void BM_Reconciler_100Symbols(benchmark::State& state)
{
  PositionReconciler reconciler;
  reconciler.setMismatchHandler([](const PositionMismatch&)
                                { return ReconcileAction::ACCEPT_EXCHANGE; });

  std::vector<ExchangePosition> exchange;
  for (int i = 0; i < 100; ++i)
  {
    exchange.push_back({static_cast<SymbolId>(i), Quantity::fromDouble(5.0 + i),
                        Price::fromDouble(100.0 + i)});
  }

  for (auto _ : state)
  {
    auto mismatches = reconciler.reconcile(exchange,
                                           [](SymbolId sym) -> std::pair<Quantity, Price>
                                           {
                                             // Every other symbol has mismatch
                                             double qty = (sym % 2 == 0) ? 5.0 + sym : 3.0 + sym;
                                             return {Quantity::fromDouble(qty), Price::fromDouble(100.0 + sym)};
                                           });
    benchmark::DoNotOptimize(mismatches.size());
  }
}
BENCHMARK(BM_Reconciler_100Symbols);

static void BM_SharedDistributor_3Trackers(benchmark::State& state)
{
  OrderSequence seq(10000);
  for (auto _ : state)
  {
    MultiExecutionListener dist{0};
    auto net = std::make_shared<MultiModePositionTracker>(1, PositionAggregationMode::NET);
    auto perSide = std::make_shared<MultiModePositionTracker>(2, PositionAggregationMode::PER_SIDE);
    auto grouped = std::make_shared<MultiModePositionTracker>(3, PositionAggregationMode::GROUPED);
    dist.addListener(net.get());
    dist.addListener(perSide.get());
    dist.addListener(grouped.get());

    for (const auto& order : seq.orders)
    {
      dist.onOrderFilled(order);
    }
    benchmark::DoNotOptimize(net->getPosition(100));
    benchmark::DoNotOptimize(perSide->getLongPosition(100));
    benchmark::DoNotOptimize(grouped->getPosition(100));
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_SharedDistributor_3Trackers);

static void BM_SharedDistributor_vs_Individual(benchmark::State& state)
{
  // Baseline: 3 individual trackers, each receiving fills separately
  OrderSequence seq(10000);
  for (auto _ : state)
  {
    MultiModePositionTracker t1{1, PositionAggregationMode::NET};
    MultiModePositionTracker t2{2, PositionAggregationMode::PER_SIDE};
    MultiModePositionTracker t3{3, PositionAggregationMode::GROUPED};

    for (const auto& order : seq.orders)
    {
      t1.onOrderFilled(order);
      t2.onOrderFilled(order);
      t3.onOrderFilled(order);
    }
    benchmark::DoNotOptimize(t1.getPosition(100));
    benchmark::DoNotOptimize(t2.getLongPosition(100));
    benchmark::DoNotOptimize(t3.getPosition(100));
  }
  state.SetItemsProcessed(state.iterations() * 10000);
}
BENCHMARK(BM_SharedDistributor_vs_Individual);

}  // namespace

BENCHMARK_MAIN();
