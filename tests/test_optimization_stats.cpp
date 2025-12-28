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

  EXPECT_NO_THROW(MockStats::generateReport(empty, reportPath));
  // File might not be created for empty results, or might contain just headers
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
  // Should not crash, just log error
  EXPECT_NO_THROW(MockStats::generateReport(results, invalidPath));
}
