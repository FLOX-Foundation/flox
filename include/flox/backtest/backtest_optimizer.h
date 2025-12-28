/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/backtest_runner.h"
#include "flox/log/log.h"
#include "flox/util/base/move_only_function.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace flox
{

enum class RankMetric
{
  SharpeRatio,
  SortinoRatio,
  CalmarRatio,
  TotalReturn,
  MaxDrawdown,
  WinRate,
  ProfitFactor
};

template <typename ParamsT>
struct OptimizationResult
{
  ParamsT parameters;
  BacktestStats stats;

  double sharpeRatio() const { return stats.sharpeRatio; }
  double sortinoRatio() const { return stats.sortinoRatio; }
  double calmarRatio() const { return stats.calmarRatio; }
  double totalReturn() const { return stats.totalPnl; }
  double maxDrawdown() const { return stats.maxDrawdown; }
  double maxDrawdownPct() const { return stats.maxDrawdownPct; }
  double winRate() const { return stats.winRate; }
  double profitFactor() const { return stats.profitFactor; }
  size_t totalTrades() const { return stats.totalTrades; }

  void setFromStats(const BacktestStats& s) { stats = s; }
};

template <typename ParamsT, typename GridT>
class BacktestOptimizer
{
 public:
  using BacktestFactory = MoveOnlyFunction<BacktestResult(const ParamsT&)>;
  using ProgressCallback = MoveOnlyFunction<void(size_t completed, size_t total, const OptimizationResult<ParamsT>& latest)>;

  BacktestOptimizer() = default;

  void setParameterGrid(const GridT& grid) { _grid = grid; }
  void setBacktestFactory(BacktestFactory factory) { _factory = std::move(factory); }
  void setProgressCallback(ProgressCallback callback) { _progressCallback = std::move(callback); }

  size_t totalCombinations() const { return _grid.totalCombinations(); }

  std::vector<OptimizationResult<ParamsT>> runLocal(size_t numThreads = 0)
  {
    if (!_factory)
    {
      FLOX_LOG_ERROR("BacktestOptimizer: No factory set");
      return {};
    }

    size_t total = _grid.totalCombinations();
    if (total == 0)
    {
      FLOX_LOG_ERROR("BacktestOptimizer: No parameter combinations");
      return {};
    }

    FLOX_LOG_INFO("Running optimization: " << total << " parameter combinations");

    if (numThreads == 0)
    {
      numThreads = std::thread::hardware_concurrency();
      if (numThreads == 0)
      {
        numThreads = 1;
      }
    }

    FLOX_LOG_INFO("Using " << numThreads << " threads");

    std::vector<OptimizationResult<ParamsT>> results(total);
    _completedTasks = 0;

    for (size_t i = 0; i < total; ++i)
    {
      ParamsT params = _grid[i];
      BacktestResult bt = _factory(params);
      auto stats = bt.computeStats();

      OptimizationResult<ParamsT>& res = results[i];
      res.parameters = params;
      res.setFromStats(stats);

      size_t completed = ++_completedTasks;
      if (_progressCallback && (completed % 10 == 0 || completed == total))
      {
        _progressCallback(completed, total, res);
      }
    }

    FLOX_LOG_INFO("Optimization complete: " << total << " backtests finished");
    return results;
  }

  OptimizationResult<ParamsT> runSingle(size_t index)
  {
    ParamsT params = _grid[index];
    BacktestResult bt = _factory(params);
    auto stats = bt.computeStats();

    OptimizationResult<ParamsT> res;
    res.parameters = params;
    res.setFromStats(stats);
    return res;
  }

  static std::vector<OptimizationResult<ParamsT>> rankResults(
      std::vector<OptimizationResult<ParamsT>> results,
      RankMetric metric,
      bool ascending = false)
  {
    auto comparator = [metric, ascending](const OptimizationResult<ParamsT>& a, const OptimizationResult<ParamsT>& b)
    {
      double valA = 0.0, valB = 0.0;
      switch (metric)
      {
        case RankMetric::SharpeRatio:
          valA = a.sharpeRatio();
          valB = b.sharpeRatio();
          break;
        case RankMetric::SortinoRatio:
          valA = a.sortinoRatio();
          valB = b.sortinoRatio();
          break;
        case RankMetric::CalmarRatio:
          valA = a.calmarRatio();
          valB = b.calmarRatio();
          break;
        case RankMetric::TotalReturn:
          valA = a.totalReturn();
          valB = b.totalReturn();
          break;
        case RankMetric::MaxDrawdown:
          valA = a.maxDrawdown();
          valB = b.maxDrawdown();
          break;
        case RankMetric::WinRate:
          valA = a.winRate();
          valB = b.winRate();
          break;
        case RankMetric::ProfitFactor:
          valA = a.profitFactor();
          valB = b.profitFactor();
          break;
      }
      return ascending ? (valA < valB) : (valA > valB);
    };

    std::sort(results.begin(), results.end(), comparator);
    return results;
  }

  template <typename Predicate>
  static std::vector<OptimizationResult<ParamsT>> filterResults(
      const std::vector<OptimizationResult<ParamsT>>& results,
      Predicate&& predicate)
  {
    std::vector<OptimizationResult<ParamsT>> filtered;
    std::copy_if(results.begin(), results.end(), std::back_inserter(filtered),
                 std::forward<Predicate>(predicate));
    return filtered;
  }

  static bool exportToCSV(const std::vector<OptimizationResult<ParamsT>>& results,
                          const std::filesystem::path& path)
  {
    std::ofstream file(path);
    if (!file.is_open())
    {
      FLOX_LOG_ERROR("Failed to open CSV file: " << path.string());
      return false;
    }

    file << "sharpe_ratio,sortino_ratio,calmar_ratio,total_return,max_drawdown,win_rate,total_trades,parameters\n";
    for (const auto& res : results)
    {
      file << res.sharpeRatio() << ","
           << res.sortinoRatio() << ","
           << res.calmarRatio() << ","
           << res.totalReturn() << ","
           << res.maxDrawdown() << ","
           << res.winRate() << ","
           << res.totalTrades() << ","
           << res.parameters.toString() << "\n";
    }
    return true;
  }

 private:
  GridT _grid;
  BacktestFactory _factory;
  ProgressCallback _progressCallback;
  std::atomic<size_t> _completedTasks{0};
};

}  // namespace flox
