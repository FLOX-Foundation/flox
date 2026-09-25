/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/events/bar_event.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/replay/ohlcv_replay_source.h"
#include "flox/strategy/abstract_strategy.h"
#include "flox/util/base/move_only_function.h"

#include <concepts>
#include <cstddef>
#include <utility>
#include <vector>

namespace flox
{

enum class WalkForwardMode
{
  /// Train window expands from the beginning: [0, t]; test [t, t + test_size].
  Anchored,
  /// Train window slides: [t - train_size, t]; test [t, t + test_size].
  Sliding,
};

struct WalkForwardConfig
{
  WalkForwardMode mode{WalkForwardMode::Anchored};
  /// Sliding mode: bars in the train window. Anchored mode: ignored.
  std::size_t trainSize{0};
  /// Bars in each test window.
  std::size_t testSize{0};
  /// Bars to advance between folds. If 0, defaults to testSize.
  std::size_t step{0};
  /// Anchored mode only: minimum bars in the train window before the
  /// first fold runs. Sliding mode: ignored.
  std::size_t minTrainSize{0};
};

struct WalkForwardFold
{
  std::size_t foldIndex{};
  std::size_t trainStartBar{};
  std::size_t trainEndBar{};
  std::size_t testStartBar{};
  std::size_t testEndBar{};
  int64_t trainStartNs{};
  int64_t trainEndNs{};
  int64_t testStartNs{};
  int64_t testEndNs{};
  BacktestStats trainStats{};
  BacktestStats testStats{};
};

/// Walk-forward orchestrator over a sequence of OHLCV close bars.
///
/// The runner builds a fresh BacktestRunner per fold and asks the user
/// for a fresh strategy via `setStrategyFactory`. Strategy state is
/// reset between folds because each fold gets a new instance from the
/// factory. Bars are converted to synthetic trade events at close
/// price (same convention as `BacktestRunner.run_csv`).
///
/// With a parameter grid and a factory that accepts parameters, each fold is
/// a walk-forward optimisation: every grid point is evaluated on the train
/// slice, the points are ranked, and the test slice runs on the winner.
/// Without them the fold is the older shape -- one unparameterised strategy
/// run on each of the two windows.
class WalkForwardRunner
{
 public:
  using StrategyFactory = MoveOnlyFunction<IStrategy*(std::size_t foldIndex)>;
  using ParameterizedStrategyFactory =
      MoveOnlyFunction<IStrategy*(std::size_t foldIndex,
                                  const std::vector<double>& params)>;

  WalkForwardRunner(const BacktestConfig& backtestConfig,
                    const WalkForwardConfig& wfConfig);

  /// One axis of candidate values per parameter. The runner walks the
  /// cartesian product (last axis varying fastest) on every fold's train
  /// slice. An empty grid -- the default -- means a single point with no
  /// parameters.
  void setParameterGrid(std::vector<std::vector<double>> axes);
  const std::vector<std::vector<double>>& parameterGrid() const noexcept
  {
    return _grid;
  }

  /// Parameterised factory: called once per grid point on the train slice,
  /// then once more with the winning point to build the out-of-sample
  /// strategy. Return nullptr to skip a point.
  template <typename F>
    requires std::invocable<F&, std::size_t, const std::vector<double>&>
  void setStrategyFactory(F&& factory)
  {
    _factory = ParameterizedStrategyFactory(std::forward<F>(factory));
    _factoryTakesParams = true;
  }

  /// Fold-index-only factory. Nothing can carry a parameter into the
  /// strategy, so the grid cannot be searched: the fold runs one strategy on
  /// the train slice and another on the test slice, as it always has. Setting
  /// a grid alongside this shape is reported and ignored.
  template <typename F>
    requires(std::invocable<F&, std::size_t> &&
             !std::invocable<F&, std::size_t, const std::vector<double>&>)
  void setStrategyFactory(F&& factory)
  {
    _factory = ParameterizedStrategyFactory(
        [inner = std::forward<F>(factory)](std::size_t foldIndex,
                                           const std::vector<double>&) mutable
        -> IStrategy*
        { return inner(foldIndex); });
    _factoryTakesParams = false;
  }

  std::vector<WalkForwardFold> run(
      const std::vector<OhlcvReplaySource::Bar>& bars);

  /// Walk-forward over a sequence of full OHLCV bars (open / high / low /
  /// close / volume). Each fold dispatches `BarEvent`s through the strategy
  /// via `BacktestRunner::runBars`, so `Strategy::onBar` fires with full
  /// bar data — high / low / volume are preserved. Use this overload for
  /// `on_bar` strategies that need intrabar information (TP/SL ladders,
  /// breakout confirmation, ATR-style indicators).
  ///
  /// Bars must be sorted by `bar.endTime`.
  std::vector<WalkForwardFold> run(const std::vector<BarEvent>& bars);

 private:
  /// The grid points to evaluate in sample: the cartesian product of the
  /// axes, or a single empty point when there is no grid to search (no axes,
  /// or a factory that cannot take parameters).
  std::vector<std::vector<double>> gridPoints() const;

  BacktestConfig _backtestConfig;
  WalkForwardConfig _wfConfig;
  ParameterizedStrategyFactory _factory;
  std::vector<std::vector<double>> _grid;
  bool _factoryTakesParams{false};
};

}  // namespace flox
