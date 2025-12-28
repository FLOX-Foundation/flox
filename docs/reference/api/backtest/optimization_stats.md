# OptimizationStatistics

Statistical utilities for analyzing optimization results.

## Overview

`OptimizationStatistics` provides statistical functions for analyzing and comparing backtest results: confidence intervals, significance tests, correlations, and reporting.

## Header

```cpp
#include "flox/backtest/optimization_stats.h"
```

## Helper Functions (detail namespace)

```cpp
namespace detail {
  double mean(const std::vector<double>& data);
  double variance(const std::vector<double>& data);
  double stddev(const std::vector<double>& data);
}
```

Basic statistical functions. Returns 0.0 for empty data.

## extractMetric

```cpp
template <typename ParamsT>
std::vector<double> extractMetric(
    const std::vector<OptimizationResult<ParamsT>>& results,
    RankMetric metric);
```

Extract specified metric values from all results.

**Example:**
```cpp
auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
double avgSharpe = detail::mean(sharpes);
```

## Class Definition

```cpp
template <typename ParamsT, typename GridT>
class OptimizationStatistics {
public:
  struct ConfidenceInterval {
    double lower;
    double median;
    double upper;
  };

  static double permutationTest(
      const std::vector<double>& group1,
      const std::vector<double>& group2,
      size_t numPermutations = 10000);

  static double correlation(
      const std::vector<double>& x,
      const std::vector<double>& y);

  static ConfidenceInterval bootstrapCI(
      const std::vector<double>& data,
      double confidenceLevel = 0.95,
      size_t numSamples = 10000);

  static void printSummary(
      const std::vector<OptimizationResult<ParamsT>>& results);

  static void generateReport(
      const std::vector<OptimizationResult<ParamsT>>& results,
      const std::filesystem::path& outputPath);
};
```

## Methods

### permutationTest

```cpp
static double permutationTest(
    const std::vector<double>& group1,
    const std::vector<double>& group2,
    size_t numPermutations = 10000);
```

Two-sample permutation test for comparing group means. Returns p-value.

**Example:**
```cpp
// Compare two parameter configurations
std::vector<double> configA = {1.2, 1.5, 1.3};
std::vector<double> configB = {0.8, 0.9, 0.7};
double pValue = Stats::permutationTest(configA, configB);
// pValue < 0.05 suggests significant difference
```

### correlation

```cpp
static double correlation(
    const std::vector<double>& x,
    const std::vector<double>& y);
```

Pearson correlation coefficient between two vectors. Returns value in [-1, 1].

**Example:**
```cpp
auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
auto returns = extractMetric(results, RankMetric::TotalReturn);
double r = Stats::correlation(sharpes, returns);
```

### bootstrapCI

```cpp
static ConfidenceInterval bootstrapCI(
    const std::vector<double>& data,
    double confidenceLevel = 0.95,
    size_t numSamples = 10000);
```

Bootstrap confidence interval for the mean.

**Example:**
```cpp
auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
auto ci = Stats::bootstrapCI(sharpes, 0.95);
// ci.lower, ci.median, ci.upper
```

### printSummary

```cpp
static void printSummary(
    const std::vector<OptimizationResult<ParamsT>>& results);
```

Print optimization summary to log. Shows total combinations, mean/stddev Sharpe, and best result details.

### generateReport

```cpp
static void generateReport(
    const std::vector<OptimizationResult<ParamsT>>& results,
    const std::filesystem::path& outputPath);
```

Generate Markdown report with top 10 results table and statistics.

## Example

```cpp
using Stats = OptimizationStatistics<MAParams, MAGrid>;

// Run optimization
auto results = optimizer.runLocal();

// Quick summary
Stats::printSummary(results);

// Full report
Stats::generateReport(results, "optimization_report.md");

// Statistical analysis
auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
auto ci = Stats::bootstrapCI(sharpes);
std::cout << "95% CI: [" << ci.lower << ", " << ci.upper << "]\n";

// Compare top vs bottom half
auto ranked = optimizer.rankResults(results, RankMetric::SharpeRatio);
size_t mid = ranked.size() / 2;
std::vector<double> topHalf, bottomHalf;
for (size_t i = 0; i < mid; ++i) {
  topHalf.push_back(ranked[i].sharpeRatio());
  bottomHalf.push_back(ranked[mid + i].sharpeRatio());
}
double pValue = Stats::permutationTest(topHalf, bottomHalf);
```

## See Also

- [BacktestOptimizer](./backtest_optimizer.md)
- [How-to: Grid Search](../../../how-to/grid-search.md)
