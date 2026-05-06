/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/backtest/walk_forward.h"
#include "flox/error/flox_error.h"
#include "flox/replay/ohlcv_replay_source.h"
#include "strategy_bindings.h"

#include <fstream>
#include <sstream>

namespace py = pybind11;

namespace flox_py
{

inline std::vector<flox::OhlcvReplaySource::Bar> loadOhlcvCsvForWf(
    const std::string& path, flox::SymbolId symbolId)
{
  std::vector<flox::OhlcvReplaySource::Bar> bars;
  std::ifstream f(path);
  if (!f.is_open())
  {
    throw flox::FloxError(
        "E_IO_001",
        "Cannot open file: '" + path +
            "'. Check the path is correct and the file is readable.");
  }
  std::string line;
  std::getline(f, line);  // header
  while (std::getline(f, line))
  {
    if (line.empty())
    {
      continue;
    }
    std::istringstream ss(line);
    std::string tok;
    std::getline(ss, tok, ',');
    int64_t ts = std::stoll(tok);
    if (ts < static_cast<int64_t>(1e12))
    {
      ts *= 1'000'000'000LL;
    }
    else if (ts < static_cast<int64_t>(1e15))
    {
      ts *= 1'000'000LL;
    }
    else if (ts < static_cast<int64_t>(1e18))
    {
      ts *= 1'000LL;
    }
    std::getline(ss, tok, ',');  // open
    std::getline(ss, tok, ',');  // high
    std::getline(ss, tok, ',');  // low
    std::getline(ss, tok, ',');
    double c = std::stod(tok);
    flox::OhlcvReplaySource::Bar bar;
    bar.ts_ns = ts;
    bar.price_raw = flox::Price::fromDouble(c).raw();
    bar.symbol_id = symbolId;
    bars.push_back(bar);
  }
  return bars;
}

inline py::dict statsToDict(const flox::BacktestStats& s)
{
  py::dict d;
  d["total_trades"] = s.totalTrades;
  d["winning_trades"] = s.winningTrades;
  d["losing_trades"] = s.losingTrades;
  d["initial_capital"] = s.initialCapital;
  d["final_capital"] = s.finalCapital;
  d["total_pnl"] = s.totalPnl;
  d["total_fees"] = s.totalFees;
  d["net_pnl"] = s.netPnl;
  d["gross_profit"] = s.grossProfit;
  d["gross_loss"] = s.grossLoss;
  d["max_drawdown"] = s.maxDrawdown;
  d["max_drawdown_pct"] = s.maxDrawdownPct;
  d["win_rate"] = s.winRate;
  d["profit_factor"] = s.profitFactor;
  d["sharpe"] = s.sharpeRatio;
  d["sortino"] = s.sortinoRatio;
  d["return_pct"] = s.returnPct;
  return d;
}

class PyWalkForwardRunner
{
 public:
  PyWalkForwardRunner(flox::SymbolRegistry* reg,
                      double feeRate, double initialCapital,
                      const std::string& mode,
                      std::size_t trainSize, std::size_t testSize,
                      std::size_t step, std::size_t minTrainSize)
      : _reg(reg)
  {
    _bcfg.feeRate = feeRate;
    _bcfg.initialCapital = initialCapital;
    _bcfg.usePercentageFee = true;
    _wcfg.mode = (mode == "sliding") ? flox::WalkForwardMode::Sliding
                                     : flox::WalkForwardMode::Anchored;
    _wcfg.trainSize = trainSize;
    _wcfg.testSize = testSize;
    _wcfg.step = step;
    _wcfg.minTrainSize = minTrainSize;
  }

  void set_strategy_factory(py::object factory)
  {
    _factory = std::move(factory);
  }

  py::list run_csv(const std::string& path, const std::string& symbol)
  {
    if (!_factory || _factory.is_none())
    {
      throw flox::FloxError(
          "E_RUN_001",
          "WalkForwardRunner.run_csv() called before set_strategy_factory().");
    }
    auto opt = _reg->getSymbolId("", symbol);
    flox::SymbolId sid = 0;
    if (opt)
    {
      sid = *opt;
    }
    else
    {
      for (const auto& info : _reg->getAllSymbols())
      {
        if (info.symbol == symbol)
        {
          sid = info.id;
          break;
        }
      }
    }
    if (sid == 0)
    {
      throw flox::FloxError(
          "E_SYM_001",
          "Symbol '" + symbol + "' is not registered.");
    }

    auto bars = loadOhlcvCsvForWf(path, sid);

    flox::WalkForwardRunner wfr(_bcfg, _wcfg);

    // Keep strategies and their hosts alive across the whole walk-forward
    // run. Each call holds a Python ref so on_trade etc. on the wrapped
    // object stay valid until run() returns. The hosts own the
    // BridgeStrategy that the engine uses.
    std::vector<py::object> aliveStrategies;
    std::vector<std::unique_ptr<PyStrategyHost>> aliveHosts;
    uint32_t hostId = 1;
    wfr.setStrategyFactory(
        [this, &aliveStrategies, &aliveHosts, &hostId](std::size_t foldIdx)
            -> flox::IStrategy*
        {
          py::gil_scoped_acquire gil;
          py::object pyStrat = _factory(foldIdx);
          auto* strat = pyStrat.cast<PyStrategyBase*>();
          auto host = std::make_unique<PyStrategyHost>(strat, _reg, hostId++, false);
          flox::IStrategy* iface = host->bridge.get();
          aliveStrategies.push_back(std::move(pyStrat));
          aliveHosts.push_back(std::move(host));
          return iface;
        });

    auto folds = wfr.run(bars);
    py::list out;
    for (const auto& f : folds)
    {
      py::dict d;
      d["fold_index"] = f.foldIndex;
      d["train_start_bar"] = f.trainStartBar;
      d["train_end_bar"] = f.trainEndBar;
      d["test_start_bar"] = f.testStartBar;
      d["test_end_bar"] = f.testEndBar;
      d["train_start_ns"] = f.trainStartNs;
      d["train_end_ns"] = f.trainEndNs;
      d["test_start_ns"] = f.testStartNs;
      d["test_end_ns"] = f.testEndNs;
      d["train_stats"] = statsToDict(f.trainStats);
      d["test_stats"] = statsToDict(f.testStats);
      out.append(d);
    }
    return out;
  }

 private:
  flox::SymbolRegistry* _reg;
  flox::BacktestConfig _bcfg;
  flox::WalkForwardConfig _wcfg;
  py::object _factory;
};

inline void bindWalkForward(py::module_& m)
{
  py::class_<PyWalkForwardRunner>(m, "WalkForwardRunner")
      .def(py::init([](flox::SymbolRegistry* reg, double feeRate,
                       double initialCapital, const std::string& mode,
                       std::size_t trainSize, std::size_t testSize,
                       std::size_t step, std::size_t minTrainSize)
                    { return std::make_unique<PyWalkForwardRunner>(
                          reg, feeRate, initialCapital, mode,
                          trainSize, testSize, step, minTrainSize); }),
           py::arg("registry"),
           py::arg("fee_rate") = 0.0004,
           py::arg("initial_capital") = 10000.0,
           py::arg("mode") = "anchored",
           py::arg("train_size") = 0,
           py::arg("test_size") = 0,
           py::arg("step") = 0,
           py::arg("min_train_size") = 0,
           py::keep_alive<1, 2>())
      .def("set_strategy_factory",
           &PyWalkForwardRunner::set_strategy_factory,
           py::arg("factory"),
           "Callable[[int], Strategy] — receives fold_index, returns "
           "a fresh Strategy instance per fold (called twice per fold: "
           "train + test).")
      .def("run_csv", &PyWalkForwardRunner::run_csv,
           py::arg("path"), py::arg("symbol"));
}

}  // namespace flox_py
