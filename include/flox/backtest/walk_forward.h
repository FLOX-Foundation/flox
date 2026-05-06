/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/replay/ohlcv_replay_source.h"
#include "flox/strategy/abstract_strategy.h"
#include "flox/util/base/move_only_function.h"

#include <cstddef>
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
class WalkForwardRunner
{
 public:
  using StrategyFactory = MoveOnlyFunction<IStrategy*(std::size_t foldIndex)>;

  WalkForwardRunner(const BacktestConfig& backtestConfig,
                    const WalkForwardConfig& wfConfig);

  void setStrategyFactory(StrategyFactory factory);

  std::vector<WalkForwardFold> run(
      const std::vector<OhlcvReplaySource::Bar>& bars);

 private:
  BacktestConfig _backtestConfig;
  WalkForwardConfig _wfConfig;
  StrategyFactory _factory;
};

}  // namespace flox
