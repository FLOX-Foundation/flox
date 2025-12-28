# Parameter Optimization with Grid Search

Optimize strategy parameters using exhaustive grid search.

## Quick Start

```cpp
#include "flox/backtest/backtest_optimizer.h"
#include "flox/backtest/optimization_stats.h"

using namespace flox;

// 1. Define your parameters
struct MAParams {
  int fastPeriod;
  int slowPeriod;
  std::string toString() const {
    return "fast=" + std::to_string(fastPeriod) +
           ",slow=" + std::to_string(slowPeriod);
  }
};

// 2. Define parameter grid
struct MAGrid {
  std::vector<int> fastPeriods = {5, 10, 15, 20};
  std::vector<int> slowPeriods = {20, 30, 40, 50};

  size_t totalCombinations() const {
    return fastPeriods.size() * slowPeriods.size();
  }

  MAParams operator[](size_t index) const {
    return {
      fastPeriods[index / slowPeriods.size()],
      slowPeriods[index % slowPeriods.size()]
    };
  }
};

// 3. Create optimizer
BacktestOptimizer<MAParams, MAGrid> optimizer;
optimizer.setParameterGrid(MAGrid{});

// 4. Set backtest factory
optimizer.setBacktestFactory([&](const MAParams& params) {
  // Create and run your strategy with these params
  return runBacktest(params);
});

// 5. Run optimization
auto results = optimizer.runLocal();

// 6. Analyze results
auto ranked = BacktestOptimizer<MAParams, MAGrid>::rankResults(
    results, RankMetric::SharpeRatio);

std::cout << "Best params: " << ranked[0].parameters.toString() << "\n";
std::cout << "Sharpe: " << ranked[0].sharpeRatio() << "\n";
```

## Components

### BacktestOptimizer

```cpp
template <typename ParamsT, typename GridT>
class BacktestOptimizer {
  void setParameterGrid(const GridT& grid);
  void setBacktestFactory(BacktestFactory factory);
  void setProgressCallback(ProgressCallback callback);

  size_t totalCombinations() const;
  std::vector<OptimizationResult<ParamsT>> runLocal(size_t numThreads = 0);
  OptimizationResult<ParamsT> runSingle(size_t index);

  static std::vector<OptimizationResult<ParamsT>> rankResults(
      std::vector<OptimizationResult<ParamsT>> results,
      RankMetric metric,
      bool ascending = false);

  static std::vector<OptimizationResult<ParamsT>> filterResults(
      const std::vector<OptimizationResult<ParamsT>>& results,
      Predicate&& predicate);

  static bool exportToCSV(
      const std::vector<OptimizationResult<ParamsT>>& results,
      const std::filesystem::path& path);
};
```

### OptimizationResult

```cpp
template <typename ParamsT>
struct OptimizationResult {
  ParamsT parameters;
  BacktestStats stats;

  double sharpeRatio() const;
  double sortinoRatio() const;
  double calmarRatio() const;
  double totalReturn() const;
  double maxDrawdown() const;
  double winRate() const;
  double profitFactor() const;
  size_t totalTrades() const;
};
```

### RankMetric

```cpp
enum class RankMetric {
  SharpeRatio,
  SortinoRatio,
  CalmarRatio,
  TotalReturn,
  MaxDrawdown,
  WinRate,
  ProfitFactor
};
```

## Grid Requirements

Your grid type must provide:

```cpp
struct MyGrid {
  // Return total number of parameter combinations
  size_t totalCombinations() const;

  // Return parameters at index [0, totalCombinations())
  MyParams operator[](size_t index) const;
};
```

Your params type must provide:

```cpp
struct MyParams {
  // For CSV export and reporting
  std::string toString() const;
};
```

## Progress Tracking

```cpp
optimizer.setProgressCallback(
    [](size_t completed, size_t total, const OptimizationResult<MAParams>& latest) {
      std::cout << "\rProgress: " << completed << "/" << total
                << " (Sharpe: " << latest.sharpeRatio() << ")" << std::flush;
    });
```

## Filtering Results

```cpp
// Only profitable strategies with sharpe > 1.0
auto filtered = BacktestOptimizer<MAParams, MAGrid>::filterResults(
    results,
    [](const auto& r) {
      return r.totalReturn() > 0 && r.sharpeRatio() > 1.0;
    });
```

## Statistical Analysis

```cpp
using Stats = OptimizationStatistics<MAParams, MAGrid>;

// Summary with mean/stddev
Stats::printSummary(results);

// Generate markdown report
Stats::generateReport(results, "report.md");

// Extract metric values
auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
double mean = detail::mean(sharpes);
double stddev = detail::stddev(sharpes);

// Bootstrap confidence interval
auto ci = Stats::bootstrapCI(sharpes, 0.95, 10000);
// ci.lower, ci.median, ci.upper

// Correlation between metrics
auto returns = extractMetric(results, RankMetric::TotalReturn);
double corr = Stats::correlation(sharpes, returns);

// Permutation test for significance
auto group1 = /* top 10 sharpes */;
auto group2 = /* bottom 10 sharpes */;
double pValue = Stats::permutationTest(group1, group2, 10000);
```

## Export Results

```cpp
// CSV export
BacktestOptimizer<MAParams, MAGrid>::exportToCSV(ranked, "results.csv");

// Markdown report
Stats::generateReport(results, "optimization_report.md");
```

## Best Practices

1. **Avoid overfitting** - More parameters = higher overfitting risk
2. **Use walk-forward analysis** - Optimize on train, validate on test
3. **Check parameter stability** - Best params should have good neighbors
4. **Consider transaction costs** - Include realistic slippage/fees
5. **Statistical significance** - Use permutation tests to validate

## Example: Full Optimization Pipeline

```cpp
// 1. Load data
auto reader = replay::createMultiSegmentReader(dataDir, filter);

// 2. Define grid
MAGrid grid;
grid.fastPeriods = {5, 10, 15, 20, 25};
grid.slowPeriods = {20, 30, 40, 50, 60};

// 3. Setup optimizer
BacktestOptimizer<MAParams, MAGrid> optimizer;
optimizer.setParameterGrid(grid);
optimizer.setBacktestFactory([&](const MAParams& p) {
  return runStrategyBacktest(p, reader);
});

// 4. Run
auto results = optimizer.runLocal();

// 5. Rank and filter
auto ranked = BacktestOptimizer<MAParams, MAGrid>::rankResults(
    results, RankMetric::SharpeRatio);

auto stable = BacktestOptimizer<MAParams, MAGrid>::filterResults(
    ranked, [](const auto& r) { return r.totalTrades() > 30; });

// 6. Analyze top performer
const auto& best = stable[0];
std::cout << "Best: " << best.parameters.toString() << "\n";
std::cout << "Sharpe: " << best.sharpeRatio() << "\n";
std::cout << "Return: " << best.totalReturn() << "\n";
std::cout << "Trades: " << best.totalTrades() << "\n";

// 7. Export
BacktestOptimizer<MAParams, MAGrid>::exportToCSV(stable, "results.csv");
```

## See Also

- [Backtesting Guide](./backtest.md)
