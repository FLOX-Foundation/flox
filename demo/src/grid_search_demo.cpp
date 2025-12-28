/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

/**
 * Grid Search Demo
 *
 * Demonstrates how to:
 * 1. Define a parameter grid for optimization
 * 2. Use BacktestOptimizer to run parameter sweep
 * 3. Analyze and rank results
 * 4. Export results to CSV
 * 5. Use OptimizationStatistics for analysis
 */

#include "flox/backtest/backtest_optimizer.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/optimization_stats.h"

#include <iostream>
#include <random>

using namespace flox;

// =============================================================================
// Step 1: Define your strategy parameters
// =============================================================================

struct MAParams
{
  int fastPeriod;
  int slowPeriod;
  double stopLossPct;

  // Required: toString() for CSV export and reporting
  std::string toString() const
  {
    return "fast=" + std::to_string(fastPeriod) + ",slow=" + std::to_string(slowPeriod) +
           ",sl=" + std::to_string(stopLossPct);
  }
};

// =============================================================================
// Step 2: Define your parameter grid
// =============================================================================

struct MAGrid
{
  std::vector<int> fastPeriods;
  std::vector<int> slowPeriods;
  std::vector<double> stopLossPcts;

  // Required: totalCombinations() returns grid size
  size_t totalCombinations() const
  {
    return fastPeriods.size() * slowPeriods.size() * stopLossPcts.size();
  }

  // Required: operator[] returns params at index
  MAParams operator[](size_t index) const
  {
    size_t numSlow = slowPeriods.size();
    size_t numSL = stopLossPcts.size();

    size_t fastIdx = index / (numSlow * numSL);
    size_t remainder = index % (numSlow * numSL);
    size_t slowIdx = remainder / numSL;
    size_t slIdx = remainder % numSL;

    return MAParams{fastPeriods[fastIdx], slowPeriods[slowIdx], stopLossPcts[slIdx]};
  }
};

// =============================================================================
// Step 3: Create a backtest factory
// =============================================================================

// In real usage, this would run your actual strategy.
// For demo purposes, we simulate results based on parameters.
BacktestResult simulateBacktest(const MAParams& params)
{
  BacktestConfig config;
  config.initialCapital = 10000.0;
  config.feeRate = 0.001;

  BacktestResult result(config);

  // Simulate trades based on parameters
  // In reality, you'd replay market data through your strategy

  std::mt19937 rng(params.fastPeriod * 1000 + params.slowPeriod);
  std::uniform_real_distribution<> priceDist(95.0, 105.0);
  std::bernoulli_distribution winDist(0.5 + (params.slowPeriod - params.fastPeriod) * 0.01);

  int numTrades = 50 + params.fastPeriod * 2;

  for (int i = 0; i < numTrades; ++i)
  {
    double entryPrice = priceDist(rng);
    bool isWin = winDist(rng);
    double exitPrice = isWin ? entryPrice * (1.0 + params.stopLossPct * 0.01)
                             : entryPrice * (1.0 - params.stopLossPct * 0.005);

    Fill buyFill;
    buyFill.orderId = static_cast<OrderId>(i * 2);
    buyFill.symbol = 1;
    buyFill.side = Side::BUY;
    buyFill.price = Price::fromDouble(entryPrice);
    buyFill.quantity = Quantity::fromDouble(1.0);
    buyFill.timestampNs = static_cast<UnixNanos>(i * 1000000);
    result.recordFill(buyFill);

    Fill sellFill;
    sellFill.orderId = static_cast<OrderId>(i * 2 + 1);
    sellFill.symbol = 1;
    sellFill.side = Side::SELL;
    sellFill.price = Price::fromDouble(exitPrice);
    sellFill.quantity = Quantity::fromDouble(1.0);
    sellFill.timestampNs = static_cast<UnixNanos>(i * 1000000 + 500000);
    result.recordFill(sellFill);
  }

  return result;
}

int main()
{
  std::cout << "=== Grid Search Demo ===\n\n";

  // =============================================================================
  // Step 4: Configure the grid
  // =============================================================================

  MAGrid grid;
  grid.fastPeriods = {5, 10, 15, 20};
  grid.slowPeriods = {20, 30, 40, 50};
  grid.stopLossPcts = {1.0, 2.0, 3.0};

  std::cout << "Parameter grid:\n";
  std::cout << "  Fast periods: ";
  for (int p : grid.fastPeriods)
  {
    std::cout << p << " ";
  }
  std::cout << "\n  Slow periods: ";
  for (int p : grid.slowPeriods)
  {
    std::cout << p << " ";
  }
  std::cout << "\n  Stop loss %: ";
  for (double p : grid.stopLossPcts)
  {
    std::cout << p << " ";
  }
  std::cout << "\n\nTotal combinations: " << grid.totalCombinations() << "\n\n";

  // =============================================================================
  // Step 5: Run optimization
  // =============================================================================

  BacktestOptimizer<MAParams, MAGrid> optimizer;
  optimizer.setParameterGrid(grid);
  optimizer.setBacktestFactory(simulateBacktest);

  // Optional: progress callback
  optimizer.setProgressCallback(
      [](size_t completed, size_t total, const OptimizationResult<MAParams>& latest)
      {
        std::cout << "\rProgress: " << completed << "/" << total << " (Sharpe: " << latest.sharpeRatio()
                  << ")" << std::flush;
      });

  std::cout << "Running optimization...\n";
  auto results = optimizer.runLocal();
  std::cout << "\n\n";

  // =============================================================================
  // Step 6: Analyze results
  // =============================================================================

  // Print summary statistics
  using Stats = OptimizationStatistics<MAParams, MAGrid>;
  Stats::printSummary(results);

  // Rank by Sharpe ratio
  auto ranked = BacktestOptimizer<MAParams, MAGrid>::rankResults(results, RankMetric::SharpeRatio);

  std::cout << "\n=== Top 5 Parameter Sets (by Sharpe) ===\n";
  for (size_t i = 0; i < std::min(size_t(5), ranked.size()); ++i)
  {
    const auto& r = ranked[i];
    std::cout << i + 1 << ". " << r.parameters.toString() << "\n";
    std::cout << "   Sharpe: " << r.sharpeRatio() << " | Sortino: " << r.sortinoRatio()
              << " | Return: " << r.totalReturn() << " | Trades: " << r.totalTrades()
              << " | Win%: " << (r.winRate() * 100) << "%\n";
  }

  // =============================================================================
  // Step 7: Filter results
  // =============================================================================

  auto profitable = BacktestOptimizer<MAParams, MAGrid>::filterResults(
      results, [](const OptimizationResult<MAParams>& r)
      { return r.totalReturn() > 0 && r.sharpeRatio() > 0.5; });

  std::cout << "\n=== Profitable strategies (return > 0, sharpe > 0.5) ===\n";
  std::cout << "Found " << profitable.size() << " out of " << results.size() << " combinations\n";

  // =============================================================================
  // Step 8: Statistical analysis
  // =============================================================================

  auto sharpeValues = extractMetric(results, RankMetric::SharpeRatio);

  std::cout << "\n=== Sharpe Ratio Distribution ===\n";
  std::cout << "Mean: " << detail::mean(sharpeValues) << "\n";
  std::cout << "Std: " << detail::stddev(sharpeValues) << "\n";

  // Bootstrap confidence interval for best strategy
  auto topSharpes =
      extractMetric(std::vector<OptimizationResult<MAParams>>(ranked.begin(), ranked.begin() + 10),
                    RankMetric::SharpeRatio);

  auto ci = Stats::bootstrapCI(topSharpes, 0.95, 1000);
  std::cout << "\n=== 95% CI for Top 10 Sharpe ===\n";
  std::cout << "Lower: " << ci.lower << " | Median: " << ci.median << " | Upper: " << ci.upper << "\n";

  // =============================================================================
  // Step 9: Export results
  // =============================================================================

  std::filesystem::path csvPath = "grid_search_results.csv";
  if (BacktestOptimizer<MAParams, MAGrid>::exportToCSV(ranked, csvPath))
  {
    std::cout << "\nResults exported to: " << csvPath << "\n";
  }

  std::filesystem::path reportPath = "optimization_report.md";
  Stats::generateReport(results, reportPath);
  std::cout << "Report generated: " << reportPath << "\n";

  return 0;
}
