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

void WalkForwardRunner::setParameterGrid(std::vector<std::vector<double>> axes)
{
  _grid = std::move(axes);
}

std::vector<std::vector<double>> WalkForwardRunner::gridPoints() const
{
  std::vector<std::vector<double>> points;
  points.emplace_back();
  if (!_factoryTakesParams)
  {
    if (!_grid.empty())
    {
      FLOX_LOG_ERROR(
          "WalkForwardRunner: a parameter grid is set but the "
          "strategy factory takes only a fold index, so no "
          "parameter can reach the strategy; the grid is ignored");
    }
    return points;
  }
  for (const auto& axis : _grid)
  {
    if (axis.empty())
    {
      // An axis with no candidates would multiply the product by zero and
      // leave nothing to evaluate. Treat it as "this parameter is not being
      // searched" rather than cancelling the whole fold.
      FLOX_LOG_WARN("WalkForwardRunner: empty parameter axis skipped");
      continue;
    }
    std::vector<std::vector<double>> next;
    next.reserve(points.size() * axis.size());
    for (const auto& point : points)
    {
      for (double value : axis)
      {
        std::vector<double> extended = point;
        extended.push_back(value);
        next.push_back(std::move(extended));
      }
    }
    points.swap(next);
  }
  return points;
}

namespace
{

struct GridSelection
{
  std::vector<double> params;
  BacktestStats stats{};
};

// Evaluate every grid point on the train window and keep the best. Ranking is
// on net PnL -- what the window actually kept after fees -- because that is
// the number the fold reports and the one a walk-forward run is judged on. A
// tie goes to the earlier grid point, so the selection is deterministic and
// does not depend on the order the points happen to be visited in.
template <typename RunWindowFn>
GridSelection selectInSample(WalkForwardRunner::ParameterizedStrategyFactory& factory,
                             const std::vector<std::vector<double>>& points,
                             std::size_t foldIndex, RunWindowFn&& runWindow)
{
  GridSelection best;
  bool haveBest = false;
  for (const auto& point : points)
  {
    IStrategy* strategy = factory(foldIndex, point);
    if (strategy == nullptr)
    {
      continue;
    }
    const BacktestStats stats = runWindow(strategy);
    if (!haveBest || stats.netPnl > best.stats.netPnl)
    {
      best.params = point;
      best.stats = stats;
      haveBest = true;
    }
  }
  return best;
}

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

BacktestStats runWindowBars(const BacktestConfig& cfg,
                            IStrategy* strategy,
                            const std::vector<BarEvent>& bars,
                            std::size_t startBar,
                            std::size_t endBarExclusive)
{
  if (startBar >= endBarExclusive || endBarExclusive > bars.size())
  {
    return {};
  }
  BacktestRunner runner(cfg);
  runner.setStrategy(strategy);
  std::vector<BarEvent> slice(
      bars.begin() + static_cast<std::ptrdiff_t>(startBar),
      bars.begin() + static_cast<std::ptrdiff_t>(endBarExclusive));
  BacktestResult res = runner.runBars(slice);
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
  const std::vector<std::vector<double>> points = gridPoints();

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

      const GridSelection winner = selectInSample(
          _factory, points, f.foldIndex,
          [&](IStrategy* strategy)
          {
            return runWindow(_backtestConfig, strategy, bars, f.trainStartBar,
                             f.trainEndBar);
          });
      f.trainStats = winner.stats;

      // Out of sample on the winning point, built fresh so the test window
      // starts from clean strategy state.
      IStrategy* testStrat = _factory(f.foldIndex, winner.params);
      f.testStats = runWindow(_backtestConfig, testStrat, bars, f.testStartBar,
                              f.testEndBar);

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

      const GridSelection winner = selectInSample(
          _factory, points, f.foldIndex,
          [&](IStrategy* strategy)
          {
            return runWindow(_backtestConfig, strategy, bars, f.trainStartBar,
                             f.trainEndBar);
          });
      f.trainStats = winner.stats;

      // Out of sample on the winning point, built fresh so the test window
      // starts from clean strategy state.
      IStrategy* testStrat = _factory(f.foldIndex, winner.params);
      f.testStats = runWindow(_backtestConfig, testStrat, bars, f.testStartBar,
                              f.testEndBar);

      folds.push_back(f);
    }
  }

  return folds;
}

namespace
{

int64_t barEndNs(const BarEvent& b)
{
  return b.bar.endTime.time_since_epoch().count();
}

}  // namespace

std::vector<WalkForwardFold> WalkForwardRunner::run(
    const std::vector<BarEvent>& bars)
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
  const std::vector<std::vector<double>> points = gridPoints();

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
      f.trainStartNs = barEndNs(bars.front());
      f.trainEndNs = barEndNs(bars[split - 1]);
      f.testStartNs = barEndNs(bars[split]);
      f.testEndNs = barEndNs(bars[f.testEndBar - 1]);

      const GridSelection winner = selectInSample(
          _factory, points, f.foldIndex,
          [&](IStrategy* strategy)
          {
            return runWindowBars(_backtestConfig, strategy, bars, f.trainStartBar,
                                 f.trainEndBar);
          });
      f.trainStats = winner.stats;

      // Out of sample on the winning point, built fresh so the test window
      // starts from clean strategy state.
      IStrategy* testStrat = _factory(f.foldIndex, winner.params);
      f.testStats = runWindowBars(_backtestConfig, testStrat, bars, f.testStartBar,
                                  f.testEndBar);

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
      f.trainStartNs = barEndNs(bars[f.trainStartBar]);
      f.trainEndNs = barEndNs(bars[f.trainEndBar - 1]);
      f.testStartNs = barEndNs(bars[f.testStartBar]);
      f.testEndNs = barEndNs(bars[f.testEndBar - 1]);

      const GridSelection winner = selectInSample(
          _factory, points, f.foldIndex,
          [&](IStrategy* strategy)
          {
            return runWindowBars(_backtestConfig, strategy, bars, f.trainStartBar,
                                 f.trainEndBar);
          });
      f.trainStats = winner.stats;

      // Out of sample on the winning point, built fresh so the test window
      // starts from clean strategy state.
      IStrategy* testStrat = _factory(f.foldIndex, winner.params);
      f.testStats = runWindowBars(_backtestConfig, testStrat, bars, f.testStartBar,
                                  f.testEndBar);

      folds.push_back(f);
    }
  }

  return folds;
}

}  // namespace flox
