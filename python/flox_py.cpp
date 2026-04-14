// flox_py -- Python bindings for Flox.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "aggregator_bindings.h"
#include "backtest_bindings.h"
#include "book_bindings.h"
#include "composite_book_bindings.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/common.h"
#include "indicator_bindings.h"
#include "optimizer_bindings.h"
#include "position_bindings.h"
#include "profile_bindings.h"
#include "replay_bindings.h"
#include "segment_ops_bindings.h"
#include "strategy_bindings.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace py = pybind11;
using namespace flox;

#pragma pack(push, 1)
struct PySignal
{
  int64_t timestamp_ns;
  int64_t quantity_raw;  // pre-scaled: qty * 1e8
  int64_t price_raw;     // pre-scaled: price * 1e8, 0 for market
  uint8_t side;          // 0=buy, 1=sell
  uint8_t order_type;    // 0=market, 1=limit
  uint8_t _pad[6];
};
#pragma pack(pop)
static_assert(sizeof(PySignal) == 32);

#pragma pack(push, 1)
struct PyBar
{
  int64_t timestamp_ns;
  int64_t open_raw;
  int64_t high_raw;
  int64_t low_raw;
  int64_t close_raw;
  int64_t volume_raw;
};
#pragma pack(pop)
static_assert(sizeof(PyBar) == 48);

static BacktestStats executeSignals(const PyBar* bars, size_t numBars,
                                    const PySignal* signals, size_t numSignals,
                                    SymbolId symbolId, const BacktestConfig& config)
{
  SimulatedClock clock;
  SimulatedExecutor executor(clock);
  OrderId nextOrderId = 1;

  std::vector<size_t> sigOrder(numSignals);
  std::iota(sigOrder.begin(), sigOrder.end(), 0);
  std::sort(sigOrder.begin(), sigOrder.end(),
            [&](size_t a, size_t b)
            { return signals[a].timestamp_ns < signals[b].timestamp_ns; });

  size_t sigIdx = 0;

  for (size_t i = 0; i < numBars; ++i)
  {
    const auto& bar = bars[i];
    clock.advanceTo(bar.timestamp_ns);
    executor.onBar(symbolId, Price::fromRaw(bar.close_raw));

    while (sigIdx < numSignals && signals[sigOrder[sigIdx]].timestamp_ns <= bar.timestamp_ns)
    {
      const auto& sig = signals[sigOrder[sigIdx]];
      Order order{.id = nextOrderId++,
                  .side = sig.side == 0 ? Side::BUY : Side::SELL,
                  .price = Price::fromRaw(sig.price_raw),
                  .quantity = Quantity::fromRaw(sig.quantity_raw),
                  .type = sig.order_type == 0 ? OrderType::MARKET : OrderType::LIMIT,
                  .symbol = symbolId};
      executor.submitOrder(order);
      ++sigIdx;
    }
  }

  while (sigIdx < numSignals)
  {
    const auto& sig = signals[sigOrder[sigIdx]];
    Order order{.id = nextOrderId++,
                .side = sig.side == 0 ? Side::BUY : Side::SELL,
                .price = Price::fromRaw(sig.price_raw),
                .quantity = Quantity::fromRaw(sig.quantity_raw),
                .type = sig.order_type == 0 ? OrderType::MARKET : OrderType::LIMIT,
                .symbol = symbolId};
    executor.submitOrder(order);
    ++sigIdx;
  }

  BacktestResult result(config, executor.fills().size());
  for (const auto& fill : executor.fills())
  {
    result.recordFill(fill);
  }

  return result.computeStats();
}

static py::dict statsToPyDict(const BacktestStats& s)
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
  d["avg_win"] = s.avgWin;
  d["avg_loss"] = s.avgLoss;
  d["sharpe"] = s.sharpeRatio;
  d["sortino"] = s.sortinoRatio;
  d["calmar"] = s.calmarRatio;
  d["return_pct"] = s.returnPct;
  return d;
}

class Engine
{
 public:
  Engine(double initialCapital = 100000.0, double feeRate = 0.0001)
  {
    _config.initialCapital = initialCapital;
    _config.feeRate = feeRate;
    _config.usePercentageFee = true;
  }

  void loadBars(py::array_t<PyBar, py::array::c_style> bars)
  {
    auto buf = bars.request();
    _bars.resize(buf.shape[0]);
    std::memcpy(_bars.data(), buf.ptr, buf.shape[0] * sizeof(PyBar));
  }

  void loadBarsFromArrays(py::array_t<int64_t> timestamps,
                          py::array_t<double> open,
                          py::array_t<double> high,
                          py::array_t<double> low,
                          py::array_t<double> close,
                          py::array_t<double> volume)
  {
    auto n = timestamps.size();
    _bars.resize(n);
    auto ts = timestamps.unchecked<1>();
    auto o = open.unchecked<1>();
    auto h = high.unchecked<1>();
    auto l = low.unchecked<1>();
    auto c = close.unchecked<1>();
    auto v = volume.unchecked<1>();

    for (py::ssize_t i = 0; i < n; ++i)
    {
      int64_t t = ts(i);
      if (t < static_cast<int64_t>(1e12))
      {
        t *= 1'000'000'000LL;
      }
      else if (t < static_cast<int64_t>(1e15))
      {
        t *= 1'000'000LL;
      }
      else if (t < static_cast<int64_t>(1e18))
      {
        t *= 1'000LL;
      }

      _bars[i] = {.timestamp_ns = t,
                  .open_raw = Price::fromDouble(o(i)).raw(),
                  .high_raw = Price::fromDouble(h(i)).raw(),
                  .low_raw = Price::fromDouble(l(i)).raw(),
                  .close_raw = Price::fromDouble(c(i)).raw(),
                  .volume_raw = Volume::fromDouble(v(i)).raw()};
    }
  }

  py::dict run(py::array_t<PySignal, py::array::c_style> signals, uint32_t symbol = 1)
  {
    auto buf = signals.request();
    const auto* sigs = static_cast<const PySignal*>(buf.ptr);
    size_t n = buf.shape[0];

    BacktestStats stats;
    {
      py::gil_scoped_release release;
      stats = executeSignals(_bars.data(), _bars.size(), sigs, n, symbol, _config);
    }
    return statsToPyDict(stats);
  }

  py::list runBatch(py::list signalSets, int threads = 0, uint32_t symbol = 1)
  {
    size_t numRuns = signalSets.size();
    if (threads <= 0)
    {
      threads = static_cast<int>(std::thread::hardware_concurrency());
    }

    struct RunData
    {
      const PySignal* data;
      size_t count;
    };
    std::vector<RunData> runs(numRuns);
    std::vector<py::array_t<PySignal, py::array::c_style>> refs(numRuns);

    for (size_t i = 0; i < numRuns; ++i)
    {
      refs[i] = signalSets[i].cast<py::array_t<PySignal, py::array::c_style>>();
      auto buf = refs[i].request();
      runs[i] = {static_cast<const PySignal*>(buf.ptr), static_cast<size_t>(buf.shape[0])};
    }

    std::vector<BacktestStats> results(numRuns);

    {
      py::gil_scoped_release release;

      std::atomic<size_t> nextRun{0};
      auto worker = [&]()
      {
        while (true)
        {
          size_t idx = nextRun.fetch_add(1, std::memory_order_relaxed);
          if (idx >= numRuns)
          {
            break;
          }
          results[idx] = executeSignals(
              _bars.data(), _bars.size(),
              runs[idx].data, runs[idx].count,
              symbol, _config);
        }
      };

      int actualThreads = std::min(threads, static_cast<int>(numRuns));
      std::vector<std::thread> pool;
      pool.reserve(actualThreads);
      for (int t = 0; t < actualThreads; ++t)
      {
        pool.emplace_back(worker);
      }
      for (auto& t : pool)
      {
        t.join();
      }
    }

    py::list out;
    for (size_t i = 0; i < numRuns; ++i)
    {
      out.append(statsToPyDict(results[i]));
    }
    return out;
  }

  size_t barCount() const { return _bars.size(); }

 private:
  BacktestConfig _config;
  std::vector<PyBar> _bars;
};

PYBIND11_MODULE(flox_py, m)
{
  m.doc() = "Flox -- Python bindings";

  PYBIND11_NUMPY_DTYPE(PySignal, timestamp_ns, quantity_raw, price_raw, side, order_type);
  PYBIND11_NUMPY_DTYPE(PyBar, timestamp_ns, open_raw, high_raw, low_raw, close_raw, volume_raw);

  py::class_<Engine>(m, "Engine")
      .def(py::init<double, double>(),
           py::arg("initial_capital") = 100000.0,
           py::arg("fee_rate") = 0.0001)
      .def("load_bars", &Engine::loadBars,
           "Load bars from numpy structured array (PyBar dtype)")
      .def("load_bars_df", &Engine::loadBarsFromArrays,
           "Load bars from DataFrame columns",
           py::arg("timestamps"), py::arg("open"), py::arg("high"),
           py::arg("low"), py::arg("close"), py::arg("volume"))
      .def("run", &Engine::run,
           "Run single backtest, returns stats dict",
           py::arg("signals"), py::arg("symbol") = 1)
      .def("run_batch", &Engine::runBatch,
           "Run N backtests in parallel C++ threads",
           py::arg("signal_sets"), py::arg("threads") = 0, py::arg("symbol") = 1)
      .def_property_readonly("bar_count", &Engine::barCount);

  m.def("make_signals", [](py::array_t<int64_t> timestamps, py::array_t<uint8_t> sides, py::array_t<double> quantities, std::optional<py::array_t<double>> prices, std::optional<py::array_t<uint8_t>> types) -> py::array_t<PySignal>
        {
          auto n = timestamps.size();
          auto ts = timestamps.unchecked<1>();
          auto sd = sides.unchecked<1>();
          auto qt = quantities.unchecked<1>();

          bool hasPrices = prices.has_value() && prices->size() == n;
          bool hasTypes = types.has_value() && types->size() == n;

          auto result = py::array_t<PySignal>(n);
          auto buf = result.mutable_unchecked<1>();

          using DblAccessor = py::detail::unchecked_reference<double, 1>;
          using U8Accessor = py::detail::unchecked_reference<uint8_t, 1>;
          std::optional<DblAccessor> px;
          std::optional<U8Accessor> tp;
          if (hasPrices){ px.emplace(prices->unchecked<1>());
}
          if (hasTypes){ tp.emplace(types->unchecked<1>());
}

          for (py::ssize_t i = 0; i < n; ++i)
          {
            int64_t t = ts(i);
            if (t < static_cast<int64_t>(1e12)){ t *= 1'000'000'000LL;
}
            else if (t < static_cast<int64_t>(1e15)){ t *= 1'000'000LL;
}
            else if (t < static_cast<int64_t>(1e18)){ t *= 1'000LL;
}

            buf(i).timestamp_ns = t;
            buf(i).quantity_raw = Quantity::fromDouble(qt(i)).raw();
            buf(i).price_raw = px ? Price::fromDouble((*px)(i)).raw() : 0;
            buf(i).side = sd(i);
            buf(i).order_type = tp ? (*tp)(i) : 0;  // default: market
          }
          return result; }, "Create signal array from separate numpy arrays", py::arg("timestamps"), py::arg("sides"), py::arg("quantities"), py::arg("prices") = py::none(), py::arg("types") = py::none());

  bindIndicators(m);
  bindAggregators(m);
  bindBooks(m);
  bindProfiles(m);
  bindPositions(m);
  bindReplay(m);
  bindSegmentOps(m);
  bindBacktest(m);
  bindOptimizer(m);
  bindCompositeBook(m);
  bindStrategy(m);
}
