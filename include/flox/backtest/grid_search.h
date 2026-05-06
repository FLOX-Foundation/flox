/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/backtest_result.h"
#include "flox/util/base/move_only_function.h"

#include <cstddef>
#include <vector>

namespace flox
{

struct GridSearchResult
{
  std::size_t paramIndex{};
  std::vector<double> params;
  BacktestStats stats;
};

/// Type-erased grid search over numeric parameter axes.
///
/// Unlike `BacktestOptimizer<ParamsT, GridT>` (template-based, C++
/// only), this version stores axes as `vector<vector<double>>` so it
/// can cross language boundaries through the C ABI. The user-supplied
/// factory builds a `BacktestResult` from a plain `vector<double>`
/// of parameter values — one slot per axis, in the same order as
/// `addAxis` calls.
class GridSearch
{
 public:
  /// Build a BacktestResult from one parameter point.
  using BacktestFactory =
      MoveOnlyFunction<BacktestResult(const std::vector<double>& params)>;

  GridSearch() = default;

  /// Append an axis of values. Each call adds one parameter dimension.
  void addAxis(std::vector<double> values);

  void setFactory(BacktestFactory factory);

  /// Total number of parameter combinations (product of axis lengths).
  std::size_t totalCombinations() const;

  /// Resolve a flat index back into a vector of parameter values, one
  /// entry per axis in registration order.
  std::vector<double> paramsForIndex(std::size_t index) const;

  /// Run the grid sequentially. Returns one result per combination,
  /// in flat-index order. `numThreads` is currently unused (parallel
  /// backend is W6-T002 vector mode multi-process).
  std::vector<GridSearchResult> run(std::size_t numThreads = 0);

 private:
  std::vector<std::vector<double>> _axes;
  BacktestFactory _factory;
};

}  // namespace flox
