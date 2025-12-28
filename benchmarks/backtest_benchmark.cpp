/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_optimizer.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/optimization_stats.h"

#include <benchmark/benchmark.h>
#include <random>

using namespace flox;

// =============================================================================
// BacktestResult benchmarks
// =============================================================================

static void BM_BacktestResult_RecordFill(benchmark::State& state)
{
  BacktestConfig config;
  BacktestResult result(config);

  OrderId orderId = 0;
  UnixNanos ts = 0;

  for (auto _ : state)
  {
    Fill fill;
    fill.orderId = ++orderId;
    fill.symbol = 1;
    fill.side = (orderId % 2 == 0) ? Side::BUY : Side::SELL;
    fill.price = Price::fromDouble(100.0);
    fill.quantity = Quantity::fromDouble(1.0);
    fill.timestampNs = ts++;

    result.recordFill(fill);
  }
}
BENCHMARK(BM_BacktestResult_RecordFill);

static void BM_BacktestResult_ComputeStats(benchmark::State& state)
{
  size_t numTrades = static_cast<size_t>(state.range(0));

  BacktestConfig config;
  BacktestResult result(config);

  std::mt19937 rng(42);
  std::uniform_real_distribution<> priceDist(95.0, 105.0);

  // Pre-populate trades
  for (size_t i = 0; i < numTrades; ++i)
  {
    Fill buyFill;
    buyFill.orderId = static_cast<OrderId>(i * 2);
    buyFill.symbol = 1;
    buyFill.side = Side::BUY;
    buyFill.price = Price::fromDouble(priceDist(rng));
    buyFill.quantity = Quantity::fromDouble(1.0);
    buyFill.timestampNs = static_cast<UnixNanos>(i * 2);
    result.recordFill(buyFill);

    Fill sellFill;
    sellFill.orderId = static_cast<OrderId>(i * 2 + 1);
    sellFill.symbol = 1;
    sellFill.side = Side::SELL;
    sellFill.price = Price::fromDouble(priceDist(rng));
    sellFill.quantity = Quantity::fromDouble(1.0);
    sellFill.timestampNs = static_cast<UnixNanos>(i * 2 + 1);
    result.recordFill(sellFill);
  }

  for (auto _ : state)
  {
    auto stats = result.computeStats();
    benchmark::DoNotOptimize(stats);
  }
}
BENCHMARK(BM_BacktestResult_ComputeStats)->Range(10, 10000);

// =============================================================================
// Optimization stats benchmarks
// =============================================================================

static void BM_OptimizationStats_Mean(benchmark::State& state)
{
  size_t size = static_cast<size_t>(state.range(0));

  std::vector<double> data(size);
  std::mt19937 rng(42);
  std::uniform_real_distribution<> dist(-100.0, 100.0);
  for (auto& v : data)
  {
    v = dist(rng);
  }

  for (auto _ : state)
  {
    double m = detail::mean(data);
    benchmark::DoNotOptimize(m);
  }
}
BENCHMARK(BM_OptimizationStats_Mean)->Range(10, 100000);

static void BM_OptimizationStats_Variance(benchmark::State& state)
{
  size_t size = static_cast<size_t>(state.range(0));

  std::vector<double> data(size);
  std::mt19937 rng(42);
  std::uniform_real_distribution<> dist(-100.0, 100.0);
  for (auto& v : data)
  {
    v = dist(rng);
  }

  for (auto _ : state)
  {
    double v = detail::variance(data);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_OptimizationStats_Variance)->Range(10, 100000);

// Test params
struct BenchParams
{
  int param1{0};
  int param2{0};
  std::string toString() const { return std::to_string(param1) + "," + std::to_string(param2); }
};

struct BenchGrid
{
  size_t size{0};
  size_t totalCombinations() const { return size; }
  BenchParams operator[](size_t i) const
  {
    return BenchParams{static_cast<int>(i / 10), static_cast<int>(i % 10)};
  }
};

static void BM_OptimizationStats_ExtractMetric(benchmark::State& state)
{
  size_t size = static_cast<size_t>(state.range(0));

  std::vector<OptimizationResult<BenchParams>> results(size);
  std::mt19937 rng(42);
  std::uniform_real_distribution<> dist(-2.0, 3.0);

  for (auto& r : results)
  {
    r.stats.sharpeRatio = dist(rng);
  }

  for (auto _ : state)
  {
    auto values = extractMetric(results, RankMetric::SharpeRatio);
    benchmark::DoNotOptimize(values);
  }
}
BENCHMARK(BM_OptimizationStats_ExtractMetric)->Range(10, 10000);

using BenchStats = OptimizationStatistics<BenchParams, BenchGrid>;

static void BM_OptimizationStats_Correlation(benchmark::State& state)
{
  size_t size = static_cast<size_t>(state.range(0));

  std::vector<double> x(size), y(size);
  std::mt19937 rng(42);
  std::uniform_real_distribution<> dist(-100.0, 100.0);

  for (size_t i = 0; i < size; ++i)
  {
    x[i] = dist(rng);
    y[i] = x[i] + dist(rng) * 0.1;  // Correlated
  }

  for (auto _ : state)
  {
    double corr = BenchStats::correlation(x, y);
    benchmark::DoNotOptimize(corr);
  }
}
BENCHMARK(BM_OptimizationStats_Correlation)->Range(10, 10000);

static void BM_OptimizationStats_PermutationTest(benchmark::State& state)
{
  size_t groupSize = static_cast<size_t>(state.range(0));

  std::vector<double> group1(groupSize), group2(groupSize);
  std::mt19937 rng(42);
  std::uniform_real_distribution<> dist1(0.0, 1.0);
  std::uniform_real_distribution<> dist2(0.5, 1.5);

  for (size_t i = 0; i < groupSize; ++i)
  {
    group1[i] = dist1(rng);
    group2[i] = dist2(rng);
  }

  for (auto _ : state)
  {
    // Use fewer permutations for benchmarking
    double pValue = BenchStats::permutationTest(group1, group2, 100);
    benchmark::DoNotOptimize(pValue);
  }
}
BENCHMARK(BM_OptimizationStats_PermutationTest)->Range(10, 100);

static void BM_OptimizationStats_BootstrapCI(benchmark::State& state)
{
  size_t size = static_cast<size_t>(state.range(0));

  std::vector<double> data(size);
  std::mt19937 rng(42);
  std::uniform_real_distribution<> dist(0.0, 100.0);

  for (auto& v : data)
  {
    v = dist(rng);
  }

  for (auto _ : state)
  {
    // Use fewer samples for benchmarking
    auto ci = BenchStats::bootstrapCI(data, 0.95, 100);
    benchmark::DoNotOptimize(ci);
  }
}
BENCHMARK(BM_OptimizationStats_BootstrapCI)->Range(10, 1000);

// =============================================================================
// BacktestOptimizer benchmarks
// =============================================================================

static void BM_BacktestOptimizer_RankResults(benchmark::State& state)
{
  size_t size = static_cast<size_t>(state.range(0));

  std::vector<OptimizationResult<BenchParams>> results(size);
  std::mt19937 rng(42);
  std::uniform_real_distribution<> dist(-2.0, 3.0);

  for (size_t i = 0; i < size; ++i)
  {
    results[i].parameters.param1 = static_cast<int>(i);
    results[i].stats.sharpeRatio = dist(rng);
  }

  for (auto _ : state)
  {
    auto ranked = BacktestOptimizer<BenchParams, BenchGrid>::rankResults(results, RankMetric::SharpeRatio);
    benchmark::DoNotOptimize(ranked);
  }
}
BENCHMARK(BM_BacktestOptimizer_RankResults)->Range(10, 10000);

static void BM_BacktestOptimizer_FilterResults(benchmark::State& state)
{
  size_t size = static_cast<size_t>(state.range(0));

  std::vector<OptimizationResult<BenchParams>> results(size);
  std::mt19937 rng(42);
  std::uniform_real_distribution<> dist(-2.0, 3.0);

  for (size_t i = 0; i < size; ++i)
  {
    results[i].stats.sharpeRatio = dist(rng);
  }

  for (auto _ : state)
  {
    auto filtered = BacktestOptimizer<BenchParams, BenchGrid>::filterResults(
        results, [](const OptimizationResult<BenchParams>& r)
        { return r.sharpeRatio() > 1.0; });
    benchmark::DoNotOptimize(filtered);
  }
}
BENCHMARK(BM_BacktestOptimizer_FilterResults)->Range(10, 10000);

static void BM_BacktestOptimizer_RunLocal(benchmark::State& state)
{
  size_t gridSize = static_cast<size_t>(state.range(0));

  for (auto _ : state)
  {
    BacktestOptimizer<BenchParams, BenchGrid> optimizer;

    BenchGrid grid;
    grid.size = gridSize;
    optimizer.setParameterGrid(grid);

    optimizer.setBacktestFactory(
        [](const BenchParams& params)
        {
          BacktestConfig config;
          BacktestResult result(config);

          // Minimal backtest simulation
          Fill buyFill;
          buyFill.orderId = 1;
          buyFill.symbol = 1;
          buyFill.side = Side::BUY;
          buyFill.price = Price::fromDouble(100.0);
          buyFill.quantity = Quantity::fromDouble(1.0);
          buyFill.timestampNs = 1000;
          result.recordFill(buyFill);

          Fill sellFill;
          sellFill.orderId = 2;
          sellFill.symbol = 1;
          sellFill.side = Side::SELL;
          sellFill.price = Price::fromDouble(100.0 + params.param1);
          sellFill.quantity = Quantity::fromDouble(1.0);
          sellFill.timestampNs = 2000;
          result.recordFill(sellFill);

          return result;
        });

    auto results = optimizer.runLocal();
    benchmark::DoNotOptimize(results);
  }
}
BENCHMARK(BM_BacktestOptimizer_RunLocal)->Range(10, 1000);

BENCHMARK_MAIN();
