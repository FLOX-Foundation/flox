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
#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/bar_replay_source.h"
#include "flox/strategy/abstract_strategy.h"

#include <memory>

namespace flox
{

template <typename ParamsT, typename GridT>
class GridSearchRunner
{
 public:
  GridSearchRunner(IBarReplaySource& barSource, const GridT& grid,
                   const BacktestConfig& config = {})
      : _barSource(barSource), _grid(grid), _config(config)
  {
    _optimizer.setParameterGrid(grid);
  }

  // StrategyFactory must return std::unique_ptr<T> where T implements IStrategy
  template <typename StrategyFactory>
  std::vector<OptimizationResult<ParamsT>> run(StrategyFactory&& strategyFactory, size_t numThreads = 0)
  {
    auto backtestFactory = [&](const ParamsT& params) -> BacktestResult
    {
      auto strategy = strategyFactory(params);
      if (!strategy)
      {
        return BacktestResult(_config);
      }

      BacktestRunner runner(_config);
      runner.setStrategy(strategy.get());
      strategy->start();

      _barSource.replay(
          [&](const BarEvent& ev)
          {
            UnixNanos barTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      ev.bar.endTime.time_since_epoch())
                                      .count();
            runner.clock().advanceTo(barTimeNs);

            runner.executor().onBar(ev.symbol, ev.bar.close);

            strategy->onBar(ev);
          });

      strategy->stop();

      return runner.extractResult();
    };

    _optimizer.setBacktestFactory(backtestFactory);
    return _optimizer.runLocal(numThreads);
  }

  BacktestOptimizer<ParamsT, GridT>& optimizer() { return _optimizer; }

 private:
  IBarReplaySource& _barSource;
  GridT _grid;
  BacktestConfig _config;
  BacktestOptimizer<ParamsT, GridT> _optimizer;
};

}  // namespace flox
