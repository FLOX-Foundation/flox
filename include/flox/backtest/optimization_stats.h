/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/backtest_optimizer.h"

#include "flox/log/log.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <random>
#include <sstream>
#include <vector>

namespace flox
{

namespace detail
{
inline double mean(const std::vector<double>& data)
{
  if (data.empty())
  {
    return 0.0;
  }
  return std::accumulate(data.begin(), data.end(), 0.0) / static_cast<double>(data.size());
}

inline double variance(const std::vector<double>& data)
{
  if (data.size() < 2)
  {
    return 0.0;
  }
  double m = mean(data);
  double sum = 0.0;
  for (double v : data)
  {
    double diff = v - m;
    sum += diff * diff;
  }
  return sum / static_cast<double>(data.size() - 1);
}

inline double stddev(const std::vector<double>& data)
{
  return std::sqrt(variance(data));
}
}  // namespace detail

template <typename ParamsT>
inline std::vector<double> extractMetric(
    const std::vector<OptimizationResult<ParamsT>>& results,
    RankMetric metric)
{
  std::vector<double> values;
  values.reserve(results.size());

  for (const auto& res : results)
  {
    double val = 0.0;
    switch (metric)
    {
      case RankMetric::SharpeRatio:
        val = res.sharpeRatio();
        break;
      case RankMetric::SortinoRatio:
        val = res.sortinoRatio();
        break;
      case RankMetric::CalmarRatio:
        val = res.calmarRatio();
        break;
      case RankMetric::TotalReturn:
        val = res.totalReturn();
        break;
      case RankMetric::MaxDrawdown:
        val = res.maxDrawdown();
        break;
      case RankMetric::WinRate:
        val = res.winRate();
        break;
      case RankMetric::ProfitFactor:
        val = res.profitFactor();
        break;
    }
    values.push_back(val);
  }
  return values;
}

template <typename ParamsT, typename GridT>
class OptimizationStatistics
{
 public:
  struct ConfidenceInterval
  {
    double lower;
    double median;
    double upper;
  };

  static double permutationTest(
      const std::vector<double>& group1,
      const std::vector<double>& group2,
      size_t numPermutations = 10000)
  {
    if (group1.empty() || group2.empty())
    {
      return 1.0;
    }

    double observedDiff = detail::mean(group1) - detail::mean(group2);

    std::vector<double> combined;
    combined.reserve(group1.size() + group2.size());
    combined.insert(combined.end(), group1.begin(), group1.end());
    combined.insert(combined.end(), group2.begin(), group2.end());

    std::random_device rd;
    std::mt19937 gen(rd());

    size_t extremeCount = 0;
    for (size_t i = 0; i < numPermutations; ++i)
    {
      std::shuffle(combined.begin(), combined.end(), gen);

      std::vector<double> perm1(combined.begin(), combined.begin() + static_cast<std::ptrdiff_t>(group1.size()));
      std::vector<double> perm2(combined.begin() + static_cast<std::ptrdiff_t>(group1.size()), combined.end());

      double permDiff = detail::mean(perm1) - detail::mean(perm2);
      if (std::abs(permDiff) >= std::abs(observedDiff))
      {
        ++extremeCount;
      }
    }

    return static_cast<double>(extremeCount) / static_cast<double>(numPermutations);
  }

  static double correlation(const std::vector<double>& x, const std::vector<double>& y)
  {
    if (x.size() != y.size() || x.size() < 2)
    {
      return 0.0;
    }

    double meanX = detail::mean(x);
    double meanY = detail::mean(y);

    double num = 0.0, denX = 0.0, denY = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
      double dx = x[i] - meanX;
      double dy = y[i] - meanY;
      num += dx * dy;
      denX += dx * dx;
      denY += dy * dy;
    }

    if (denX == 0.0 || denY == 0.0)
    {
      return 0.0;
    }
    return num / std::sqrt(denX * denY);
  }

  static ConfidenceInterval bootstrapCI(
      const std::vector<double>& data,
      double confidenceLevel = 0.95,
      size_t numSamples = 10000)
  {
    if (data.empty())
    {
      return {0.0, 0.0, 0.0};
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, data.size() - 1);

    std::vector<double> bootstrapMeans;
    bootstrapMeans.reserve(numSamples);

    for (size_t i = 0; i < numSamples; ++i)
    {
      std::vector<double> sample;
      sample.reserve(data.size());
      for (size_t j = 0; j < data.size(); ++j)
      {
        sample.push_back(data[dist(gen)]);
      }
      bootstrapMeans.push_back(detail::mean(sample));
    }

    std::sort(bootstrapMeans.begin(), bootstrapMeans.end());

    size_t lowerIdx = static_cast<size_t>((1.0 - confidenceLevel) / 2.0 * static_cast<double>(numSamples));
    size_t upperIdx = static_cast<size_t>((1.0 + confidenceLevel) / 2.0 * static_cast<double>(numSamples));
    size_t medianIdx = numSamples / 2;

    return {bootstrapMeans[lowerIdx], bootstrapMeans[medianIdx], bootstrapMeans[upperIdx]};
  }

  static void printSummary(const std::vector<OptimizationResult<ParamsT>>& results)
  {
    if (results.empty())
    {
      FLOX_LOG_WARN("No results to summarize");
      return;
    }

    auto metricValues = extractMetric(results, RankMetric::SharpeRatio);

    auto best = std::max_element(results.begin(), results.end(),
                                 [](const auto& a, const auto& b)
                                 { return a.sharpeRatio() < b.sharpeRatio(); });

    FLOX_LOG_INFO("=== Optimization Summary ===");
    FLOX_LOG_INFO("Total combinations: " << results.size()
                                         << ", Mean Sharpe: " << detail::mean(metricValues)
                                         << ", StdDev: " << detail::stddev(metricValues));
    FLOX_LOG_INFO("Best: Sharpe=" << best->sharpeRatio()
                                  << " Sortino=" << best->sortinoRatio()
                                  << " Calmar=" << best->calmarRatio()
                                  << " Return=" << best->totalReturn()
                                  << " DD=" << best->maxDrawdown() << "%"
                                  << " WinRate=" << (best->winRate() * 100) << "%"
                                  << " Trades=" << best->totalTrades()
                                  << " Params=" << best->parameters.toString());
  }

  static void generateReport(
      const std::vector<OptimizationResult<ParamsT>>& results,
      const std::filesystem::path& outputPath)
  {
    std::ofstream file(outputPath);
    if (!file.is_open())
    {
      FLOX_LOG_ERROR("Failed to open report file: " << outputPath.string());
      return;
    }

    file << "# Optimization Report\n\n";
    file << "Total combinations: " << results.size() << "\n\n";

    file << "## Top 10 Results\n\n";
    file << "| Rank | Sharpe | Sortino | Calmar | Return | Drawdown | Win Rate | Trades | Parameters |\n";
    file << "|------|--------|---------|--------|--------|----------|----------|--------|------------|\n";

    auto sorted = BacktestOptimizer<ParamsT, GridT>::rankResults(results, RankMetric::SharpeRatio);

    for (size_t i = 0; i < std::min(size_t(10), sorted.size()); ++i)
    {
      const auto& res = sorted[i];
      file << "| " << (i + 1) << " | "
           << std::fixed << std::setprecision(2) << res.sharpeRatio() << " | "
           << res.sortinoRatio() << " | "
           << res.calmarRatio() << " | "
           << res.totalReturn() << " | "
           << res.maxDrawdown() << "% | "
           << (res.winRate() * 100) << "% | "
           << res.totalTrades() << " | "
           << res.parameters.toString() << " |\n";
    }

    file << "\n## Statistics\n\n";
    auto metricValues = extractMetric(results, RankMetric::SharpeRatio);
    file << "- Mean Sharpe: " << detail::mean(metricValues) << "\n";
    file << "- Std Dev Sharpe: " << detail::stddev(metricValues) << "\n";

    FLOX_LOG_INFO("Report generated: " << outputPath.string());
  }
};

}  // namespace flox
