/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/optimization_stats.h"

#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>

using namespace flox;

class OptimizationStatsTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _test_dir = std::filesystem::temp_directory_path() / "flox_opt_stats_test";
    std::filesystem::remove_all(_test_dir);
    std::filesystem::create_directories(_test_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_test_dir); }

  std::filesystem::path _test_dir;
};

// detail::mean tests
TEST_F(OptimizationStatsTest, MeanEmpty)
{
  std::vector<double> empty;
  EXPECT_DOUBLE_EQ(detail::mean(empty), 0.0);
}

TEST_F(OptimizationStatsTest, MeanSingleValue)
{
  std::vector<double> single = {42.0};
  EXPECT_DOUBLE_EQ(detail::mean(single), 42.0);
}

TEST_F(OptimizationStatsTest, MeanMultipleValues)
{
  std::vector<double> values = {1.0, 2.0, 3.0, 4.0, 5.0};
  EXPECT_DOUBLE_EQ(detail::mean(values), 3.0);
}

TEST_F(OptimizationStatsTest, MeanNegativeValues)
{
  std::vector<double> values = {-10.0, 0.0, 10.0};
  EXPECT_DOUBLE_EQ(detail::mean(values), 0.0);
}

// detail::variance tests
TEST_F(OptimizationStatsTest, VarianceEmpty)
{
  std::vector<double> empty;
  EXPECT_DOUBLE_EQ(detail::variance(empty), 0.0);
}

TEST_F(OptimizationStatsTest, VarianceSingleValue)
{
  std::vector<double> single = {42.0};
  EXPECT_DOUBLE_EQ(detail::variance(single), 0.0);
}

TEST_F(OptimizationStatsTest, VarianceUniform)
{
  std::vector<double> values = {5.0, 5.0, 5.0, 5.0};
  EXPECT_DOUBLE_EQ(detail::variance(values), 0.0);
}

TEST_F(OptimizationStatsTest, VarianceKnownValue)
{
  // 1, 2, 3, 4, 5 -> mean=3, variance = ((4+1+0+1+4)/4) = 2.5 (sample variance)
  std::vector<double> values = {1.0, 2.0, 3.0, 4.0, 5.0};
  EXPECT_DOUBLE_EQ(detail::variance(values), 2.5);
}

// detail::stddev tests
TEST_F(OptimizationStatsTest, StddevEmpty)
{
  std::vector<double> empty;
  EXPECT_DOUBLE_EQ(detail::stddev(empty), 0.0);
}

TEST_F(OptimizationStatsTest, StddevKnownValue)
{
  std::vector<double> values = {1.0, 2.0, 3.0, 4.0, 5.0};
  EXPECT_DOUBLE_EQ(detail::stddev(values), std::sqrt(2.5));
}

// Mock params for testing
struct MockParams
{
  int fastPeriod{10};
  int slowPeriod{20};

  std::string toString() const
  {
    return "fast=" + std::to_string(fastPeriod) + ",slow=" + std::to_string(slowPeriod);
  }
};

// extractMetric tests
TEST_F(OptimizationStatsTest, ExtractMetricSharpe)
{
  std::vector<OptimizationResult<MockParams>> results;

  OptimizationResult<MockParams> r1;
  r1.stats.sharpeRatio = 1.5;
  results.push_back(r1);

  OptimizationResult<MockParams> r2;
  r2.stats.sharpeRatio = 2.0;
  results.push_back(r2);

  OptimizationResult<MockParams> r3;
  r3.stats.sharpeRatio = 0.5;
  results.push_back(r3);

  auto values = extractMetric(results, RankMetric::SharpeRatio);
  ASSERT_EQ(values.size(), 3);
  EXPECT_DOUBLE_EQ(values[0], 1.5);
  EXPECT_DOUBLE_EQ(values[1], 2.0);
  EXPECT_DOUBLE_EQ(values[2], 0.5);
}

TEST_F(OptimizationStatsTest, ExtractMetricWinRate)
{
  std::vector<OptimizationResult<MockParams>> results;

  OptimizationResult<MockParams> r1;
  r1.stats.winRate = 0.6;
  results.push_back(r1);

  OptimizationResult<MockParams> r2;
  r2.stats.winRate = 0.55;
  results.push_back(r2);

  auto values = extractMetric(results, RankMetric::WinRate);
  ASSERT_EQ(values.size(), 2);
  EXPECT_DOUBLE_EQ(values[0], 0.6);
  EXPECT_DOUBLE_EQ(values[1], 0.55);
}

TEST_F(OptimizationStatsTest, ExtractMetricAllTypes)
{
  std::vector<OptimizationResult<MockParams>> results;

  OptimizationResult<MockParams> r;
  r.stats.sharpeRatio = 1.0;
  r.stats.sortinoRatio = 1.5;
  r.stats.calmarRatio = 2.0;
  r.stats.totalPnl = 1000.0;
  r.stats.maxDrawdown = 100.0;
  r.stats.winRate = 0.6;
  r.stats.profitFactor = 2.5;
  results.push_back(r);

  EXPECT_DOUBLE_EQ(extractMetric(results, RankMetric::SharpeRatio)[0], 1.0);
  EXPECT_DOUBLE_EQ(extractMetric(results, RankMetric::SortinoRatio)[0], 1.5);
  EXPECT_DOUBLE_EQ(extractMetric(results, RankMetric::CalmarRatio)[0], 2.0);
  EXPECT_DOUBLE_EQ(extractMetric(results, RankMetric::TotalReturn)[0], 1000.0);
  EXPECT_DOUBLE_EQ(extractMetric(results, RankMetric::MaxDrawdown)[0], 100.0);
  EXPECT_DOUBLE_EQ(extractMetric(results, RankMetric::WinRate)[0], 0.6);
  EXPECT_DOUBLE_EQ(extractMetric(results, RankMetric::ProfitFactor)[0], 2.5);
}

// Mock grid for testing
struct MockGrid
{
  std::vector<MockParams> params;

  size_t totalCombinations() const { return params.size(); }
  MockParams operator[](size_t i) const { return params[i]; }
};

using MockStats = OptimizationStatistics<MockParams, MockGrid>;

// Correlation tests
TEST_F(OptimizationStatsTest, CorrelationPerfectPositive)
{
  std::vector<double> x = {1.0, 2.0, 3.0, 4.0, 5.0};
  std::vector<double> y = {2.0, 4.0, 6.0, 8.0, 10.0};

  double corr = MockStats::correlation(x, y);
  EXPECT_NEAR(corr, 1.0, 1e-10);
}

TEST_F(OptimizationStatsTest, CorrelationPerfectNegative)
{
  std::vector<double> x = {1.0, 2.0, 3.0, 4.0, 5.0};
  std::vector<double> y = {10.0, 8.0, 6.0, 4.0, 2.0};

  double corr = MockStats::correlation(x, y);
  EXPECT_NEAR(corr, -1.0, 1e-10);
}

TEST_F(OptimizationStatsTest, CorrelationZero)
{
  std::vector<double> x = {1.0, 2.0, 3.0, 4.0, 5.0};
  std::vector<double> y = {5.0, 5.0, 5.0, 5.0, 5.0};  // No variance

  double corr = MockStats::correlation(x, y);
  EXPECT_DOUBLE_EQ(corr, 0.0);
}

TEST_F(OptimizationStatsTest, CorrelationDifferentSizes)
{
  std::vector<double> x = {1.0, 2.0, 3.0};
  std::vector<double> y = {1.0, 2.0};

  double corr = MockStats::correlation(x, y);
  EXPECT_DOUBLE_EQ(corr, 0.0);
}

TEST_F(OptimizationStatsTest, CorrelationEmpty)
{
  std::vector<double> x;
  std::vector<double> y;

  double corr = MockStats::correlation(x, y);
  EXPECT_DOUBLE_EQ(corr, 0.0);
}

// Permutation test
TEST_F(OptimizationStatsTest, PermutationTestIdenticalGroups)
{
  std::vector<double> group1 = {1.0, 2.0, 3.0, 4.0, 5.0};
  std::vector<double> group2 = {1.0, 2.0, 3.0, 4.0, 5.0};

  double pValue = MockStats::permutationTest(group1, group2, 1000);
  // Identical groups should have p-value = 1.0 (no significant difference)
  EXPECT_NEAR(pValue, 1.0, 0.05);
}

TEST_F(OptimizationStatsTest, PermutationTestDifferentGroups)
{
  std::vector<double> group1 = {1.0, 2.0, 3.0, 4.0, 5.0};
  std::vector<double> group2 = {100.0, 101.0, 102.0, 103.0, 104.0};

  double pValue = MockStats::permutationTest(group1, group2, 1000);
  // Very different groups should have low p-value
  EXPECT_LT(pValue, 0.05);
}

TEST_F(OptimizationStatsTest, PermutationTestEmptyGroups)
{
  std::vector<double> group1;
  std::vector<double> group2 = {1.0, 2.0};

  double pValue = MockStats::permutationTest(group1, group2, 100);
  EXPECT_DOUBLE_EQ(pValue, 1.0);
}

// Bootstrap CI
TEST_F(OptimizationStatsTest, BootstrapCIEmpty)
{
  std::vector<double> empty;
  auto ci = MockStats::bootstrapCI(empty, 0.95, 100);

  EXPECT_DOUBLE_EQ(ci.lower, 0.0);
  EXPECT_DOUBLE_EQ(ci.median, 0.0);
  EXPECT_DOUBLE_EQ(ci.upper, 0.0);
}

TEST_F(OptimizationStatsTest, BootstrapCISingleValue)
{
  std::vector<double> single = {42.0};
  auto ci = MockStats::bootstrapCI(single, 0.95, 100);

  EXPECT_DOUBLE_EQ(ci.lower, 42.0);
  EXPECT_DOUBLE_EQ(ci.median, 42.0);
  EXPECT_DOUBLE_EQ(ci.upper, 42.0);
}

TEST_F(OptimizationStatsTest, BootstrapCIMultipleValues)
{
  std::vector<double> values = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0};
  auto ci = MockStats::bootstrapCI(values, 0.95, 1000);

  // Mean is 5.5, CI should contain it
  EXPECT_LT(ci.lower, 5.5);
  EXPECT_GT(ci.upper, 5.5);
  EXPECT_NEAR(ci.median, 5.5, 1.0);

  // Lower should be less than upper
  EXPECT_LT(ci.lower, ci.upper);
}

// === BT-07 / TD-06: determinism, add-one p-value correction, OOB safety ===

// Before the fix, both permutationTest and bootstrapCI seeded their RNG from
// `std::random_device` inside the function body -- the same call on the same
// data could read differently on every invocation. The reproducer (`det2.cpp`
// in the audit) called permutationTest 200 times on identical, well-mixed
// data at the p<0.001 decision threshold and got "significant" 129 times and
// "not significant" 71 times. With a fixed default seed, repeated calls on
// identical input must now be bit-for-bit identical.
TEST_F(OptimizationStatsTest, PermutationTestIsDeterministicAcrossCalls)
{
  std::vector<double> group1 = {1.0, 5.0, 2.0, 9.0, 3.0, 7.0, 4.0, 6.0};
  std::vector<double> group2 = {2.0, 6.0, 3.0, 8.0, 1.0, 9.0, 5.0, 4.0};

  const double first = MockStats::permutationTest(group1, group2, 2000);
  for (int i = 0; i < 20; ++i)
  {
    EXPECT_DOUBLE_EQ(MockStats::permutationTest(group1, group2, 2000), first);
  }
}

TEST_F(OptimizationStatsTest, BootstrapCIIsDeterministicAcrossCalls)
{
  std::vector<double> values = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0};

  const auto first = MockStats::bootstrapCI(values, 0.95, 500);
  for (int i = 0; i < 20; ++i)
  {
    const auto ci = MockStats::bootstrapCI(values, 0.95, 500);
    EXPECT_DOUBLE_EQ(ci.lower, first.lower);
    EXPECT_DOUBLE_EQ(ci.median, first.median);
    EXPECT_DOUBLE_EQ(ci.upper, first.upper);
  }
}

// A caller who explicitly wants independent resamples across calls can still
// get them by varying the seed.
TEST_F(OptimizationStatsTest, PermutationTestSeedParameterVariesResampling)
{
  std::vector<double> group1 = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
  std::vector<double> group2 = {3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0};

  bool sawDifference = false;
  double first = MockStats::permutationTest(group1, group2, 300, /*seed=*/0);
  for (std::uint64_t seed = 1; seed <= 8; ++seed)
  {
    if (MockStats::permutationTest(group1, group2, 300, seed) != first)
    {
      sawDifference = true;
      break;
    }
  }
  // Guards against the seed parameter being silently ignored: at least one
  // of a handful of different seeds must resample differently.
  EXPECT_TRUE(sawDifference);
}

// A permutation test can never honestly report p == 0.0: the observed
// arrangement is itself one of the numPermutations + 1 possible outcomes
// under the null. The old code returned extremeCount / numPermutations,
// which hits exactly 0.0 whenever no shuffled arrangement sampled is as
// extreme as the observed one -- routine for well-separated groups, where
// very few of the possible arrangements tie or exceed the observed split.
TEST_F(OptimizationStatsTest, PermutationTestNeverReturnsExactZero)
{
  std::vector<double> group1 = {1.0, 2.0, 3.0};
  std::vector<double> group2 = {100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0};

  const double pValue = MockStats::permutationTest(group1, group2, 500);
  // The add-one correction guarantees p >= 1/(numPermutations + 1) > 0 no
  // matter how few (including zero) sampled arrangements are as extreme as
  // the observed one -- the old formula (extremeCount / numPermutations)
  // returns exactly 0.0 whenever the sample happens to contain zero such
  // arrangements, which is the common case here.
  EXPECT_GT(pValue, 0.0);
  EXPECT_GE(pValue, 1.0 / 501.0 - 1e-12);
}

// BT-07: confidenceLevel == 1.0 must not read past the end of the sorted
// bootstrap-means buffer (a confirmed ASan heap-buffer-overflow before the
// fix: upperIdx == numSamples).
TEST_F(OptimizationStatsTest, BootstrapCIConfidenceLevelOneDoesNotReadOutOfBounds)
{
  std::vector<double> values = {1.0, 2.0, 3.0, 4.0, 5.0};
  auto ci = MockStats::bootstrapCI(values, /*confidenceLevel=*/1.0, 100);
  EXPECT_LE(ci.lower, ci.median);
  EXPECT_LE(ci.median, ci.upper);
}

// BT-07: numSamples == 0 must not crash (a confirmed SEGV before the fix:
// indexing into an empty bootstrapMeans vector).
TEST_F(OptimizationStatsTest, BootstrapCIZeroSamplesReturnsZeroInsteadOfCrashing)
{
  std::vector<double> values = {1.0, 2.0, 3.0};
  auto ci = MockStats::bootstrapCI(values, 0.95, /*numSamples=*/0);
  EXPECT_DOUBLE_EQ(ci.lower, 0.0);
  EXPECT_DOUBLE_EQ(ci.median, 0.0);
  EXPECT_DOUBLE_EQ(ci.upper, 0.0);
}

// printSummary test (just ensure it doesn't crash)
TEST_F(OptimizationStatsTest, PrintSummaryEmpty)
{
  std::vector<OptimizationResult<MockParams>> empty;
  // Should not crash, just log warning
  EXPECT_NO_THROW(MockStats::printSummary(empty));
}

TEST_F(OptimizationStatsTest, PrintSummaryWithResults)
{
  std::vector<OptimizationResult<MockParams>> results;

  for (int i = 0; i < 5; ++i)
  {
    OptimizationResult<MockParams> r;
    r.parameters.fastPeriod = 10 + i;
    r.parameters.slowPeriod = 20 + i;
    r.stats.sharpeRatio = 1.0 + i * 0.1;
    r.stats.sortinoRatio = 1.5 + i * 0.1;
    r.stats.calmarRatio = 2.0 + i * 0.1;
    r.stats.totalPnl = 1000.0 + i * 100;
    r.stats.maxDrawdown = 50.0 + i * 5;
    r.stats.winRate = 0.5 + i * 0.02;
    r.stats.totalTrades = 100 + i * 10;
    results.push_back(r);
  }

  EXPECT_NO_THROW(MockStats::printSummary(results));
}

// generateReport test
TEST_F(OptimizationStatsTest, GenerateReportEmpty)
{
  std::vector<OptimizationResult<MockParams>> empty;
  auto reportPath = _test_dir / "empty_report.md";

  // TD-07: generateReport now reports success/failure via its return value
  // instead of leaving the caller to infer it from the log stream. With a
  // valid (writable) path, opening the file succeeds even for zero results
  // -- the report is headers-only, not absent.
  bool wrote = false;
  EXPECT_NO_THROW(wrote = MockStats::generateReport(empty, reportPath));
  EXPECT_TRUE(wrote);
  ASSERT_TRUE(std::filesystem::exists(reportPath));

  std::ifstream file(reportPath);
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  EXPECT_TRUE(content.find("# Optimization Report") != std::string::npos);
  EXPECT_TRUE(content.find("Total combinations: 0") != std::string::npos);
}

TEST_F(OptimizationStatsTest, GenerateReportWithResults)
{
  std::vector<OptimizationResult<MockParams>> results;

  for (int i = 0; i < 15; ++i)
  {
    OptimizationResult<MockParams> r;
    r.parameters.fastPeriod = 10 + i;
    r.parameters.slowPeriod = 20 + i;
    r.stats.sharpeRatio = 1.0 + i * 0.1;
    r.stats.sortinoRatio = 1.5 + i * 0.1;
    r.stats.calmarRatio = 2.0 + i * 0.1;
    r.stats.totalPnl = 1000.0 + i * 100;
    r.stats.maxDrawdown = 50.0 + i * 5;
    r.stats.winRate = 0.5 + i * 0.02;
    r.stats.totalTrades = 100 + i * 10;
    results.push_back(r);
  }

  auto reportPath = _test_dir / "report.md";
  MockStats::generateReport(results, reportPath);

  EXPECT_TRUE(std::filesystem::exists(reportPath));

  // Check file content
  std::ifstream file(reportPath);
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

  EXPECT_TRUE(content.find("# Optimization Report") != std::string::npos);
  EXPECT_TRUE(content.find("## Top 10 Results") != std::string::npos);
  EXPECT_TRUE(content.find("## Statistics") != std::string::npos);
  EXPECT_TRUE(content.find("Mean Sharpe") != std::string::npos);
}

TEST_F(OptimizationStatsTest, GenerateReportInvalidPath)
{
  std::vector<OptimizationResult<MockParams>> results;
  OptimizationResult<MockParams> r;
  r.stats.sharpeRatio = 1.0;
  results.push_back(r);

  auto invalidPath = "/nonexistent/directory/report.md";
  // TD-07: before this fix, the only way to tell a lost report from a
  // written one was to notice the ERROR line in the log stream -- the
  // function returned void either way. It must now report failure via its
  // return value, and the file must genuinely not have been created.
  bool wrote = true;
  EXPECT_NO_THROW(wrote = MockStats::generateReport(results, invalidPath));
  EXPECT_FALSE(wrote);
  EXPECT_FALSE(std::filesystem::exists(invalidPath));
}
