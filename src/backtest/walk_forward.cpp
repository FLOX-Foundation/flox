/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/walk_forward.h"

#include "flox/backtest/backtest_runner.h"
#include "flox/log/log.h"

namespace flox
{

WalkForwardRunner::WalkForwardRunner(const BacktestConfig& backtestConfig,
                                     const WalkForwardConfig& wfConfig)
    : _backtestConfig(backtestConfig), _wfConfig(wfConfig)
{
}

void WalkForwardRunner::setStrategyFactory(StrategyFactory factory)
{
  _factory = std::move(factory);
}

namespace
{

BacktestStats runWindow(const BacktestConfig& cfg,
                        IStrategy* strategy,
                        const std::vector<OhlcvReplaySource::Bar>& bars,
                        std::size_t startBar,
                        std::size_t endBarExclusive)
{
  if (startBar >= endBarExclusive || endBarExclusive > bars.size())
  {
    return {};
  }
  BacktestRunner runner(cfg);
  runner.setStrategy(strategy);
  std::vector<OhlcvReplaySource::Bar> slice(
      bars.begin() + static_cast<std::ptrdiff_t>(startBar),
      bars.begin() + static_cast<std::ptrdiff_t>(endBarExclusive));
  OhlcvReplaySource reader(std::move(slice));
  BacktestResult res = runner.run(reader);
  return res.computeStats();
}

}  // namespace

std::vector<WalkForwardFold> WalkForwardRunner::run(
    const std::vector<OhlcvReplaySource::Bar>& bars)
{
  std::vector<WalkForwardFold> folds;
  if (!_factory)
  {
    FLOX_LOG_ERROR("WalkForwardRunner: no strategy factory set");
    return folds;
  }
  if (_wfConfig.testSize == 0)
  {
    FLOX_LOG_ERROR("WalkForwardRunner: testSize must be > 0");
    return folds;
  }
  const std::size_t step = _wfConfig.step == 0 ? _wfConfig.testSize : _wfConfig.step;
  const std::size_t n = bars.size();

  std::size_t foldIdx = 0;

  if (_wfConfig.mode == WalkForwardMode::Anchored)
  {
    const std::size_t firstSplit = _wfConfig.minTrainSize > 0
                                       ? _wfConfig.minTrainSize
                                       : _wfConfig.testSize;
    for (std::size_t split = firstSplit; split + _wfConfig.testSize <= n;
         split += step)
    {
      WalkForwardFold f{};
      f.foldIndex = foldIdx++;
      f.trainStartBar = 0;
      f.trainEndBar = split;
      f.testStartBar = split;
      f.testEndBar = split + _wfConfig.testSize;
      f.trainStartNs = bars.front().ts_ns;
      f.trainEndNs = bars[split - 1].ts_ns;
      f.testStartNs = bars[split].ts_ns;
      f.testEndNs = bars[f.testEndBar - 1].ts_ns;

      IStrategy* trainStrat = _factory(f.foldIndex);
      f.trainStats = runWindow(_backtestConfig, trainStrat, bars,
                               f.trainStartBar, f.trainEndBar);

      IStrategy* testStrat = _factory(f.foldIndex);
      f.testStats = runWindow(_backtestConfig, testStrat, bars,
                              f.testStartBar, f.testEndBar);

      folds.push_back(f);
    }
  }
  else  // Sliding
  {
    if (_wfConfig.trainSize == 0)
    {
      FLOX_LOG_ERROR("WalkForwardRunner: sliding mode requires trainSize > 0");
      return folds;
    }
    for (std::size_t trainStart = 0;
         trainStart + _wfConfig.trainSize + _wfConfig.testSize <= n;
         trainStart += step)
    {
      WalkForwardFold f{};
      f.foldIndex = foldIdx++;
      f.trainStartBar = trainStart;
      f.trainEndBar = trainStart + _wfConfig.trainSize;
      f.testStartBar = f.trainEndBar;
      f.testEndBar = f.testStartBar + _wfConfig.testSize;
      f.trainStartNs = bars[f.trainStartBar].ts_ns;
      f.trainEndNs = bars[f.trainEndBar - 1].ts_ns;
      f.testStartNs = bars[f.testStartBar].ts_ns;
      f.testEndNs = bars[f.testEndBar - 1].ts_ns;

      IStrategy* trainStrat = _factory(f.foldIndex);
      f.trainStats = runWindow(_backtestConfig, trainStrat, bars,
                               f.trainStartBar, f.trainEndBar);

      IStrategy* testStrat = _factory(f.foldIndex);
      f.testStats = runWindow(_backtestConfig, testStrat, bars,
                              f.testStartBar, f.testEndBar);

      folds.push_back(f);
    }
  }

  return folds;
}

}  // namespace flox
