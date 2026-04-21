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
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace py = pybind11;
using namespace flox;

using contiguous_f64 = py::array_t<double, py::array::c_style | py::array::forcecast>;
using contiguous_i64 = py::array_t<int64_t, py::array::c_style | py::array::forcecast>;

// ── Signal ──────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct PySignal
{
  int64_t timestamp_ns;
  int64_t quantity_raw;
  int64_t price_raw;
  uint8_t side;
  uint8_t order_type;
  uint8_t _pad[6];
};
#pragma pack(pop)
static_assert(sizeof(PySignal) == 32);

static int64_t normalizeTimestamp(int64_t t)
{
  if (t < static_cast<int64_t>(1e12))
  {
    return t * 1'000'000'000LL;
  }
  if (t < static_cast<int64_t>(1e15))
  {
    return t * 1'000'000LL;
  }
  if (t < static_cast<int64_t>(1e18))
  {
    return t * 1'000LL;
  }
  return t;
}

// ── SignalBuilder ───────────────────────────────────────────────────

class SignalBuilder
{
 public:
  void addSignal(int64_t ts, uint8_t side, double qty, double price, uint8_t type)
  {
    _signals.push_back({.timestamp_ns = normalizeTimestamp(ts),
                        .quantity_raw = Quantity::fromDouble(qty).raw(),
                        .price_raw = price > 0 ? Price::fromDouble(price).raw() : 0,
                        .side = side,
                        .order_type = type,
                        ._pad = {}});
  }

  void buy(int64_t ts, double qty) { addSignal(ts, 0, qty, 0, 0); }
  void sell(int64_t ts, double qty) { addSignal(ts, 1, qty, 0, 0); }
  void limitBuy(int64_t ts, double price, double qty) { addSignal(ts, 0, qty, price, 1); }
  void limitSell(int64_t ts, double price, double qty) { addSignal(ts, 1, qty, price, 1); }

  py::array_t<PySignal> build() const
  {
    auto result = py::array_t<PySignal>(_signals.size());
    std::memcpy(result.mutable_data(), _signals.data(), _signals.size() * sizeof(PySignal));
    return result;
  }

  size_t size() const { return _signals.size(); }
  void clear() { _signals.clear(); }

 private:
  std::vector<PySignal> _signals;
};

// ── Bar ─────────────────────────────────────────────────────────────

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

// ── Stats ───────────────────────────────────────────────────────────

struct PyStats
{
  size_t total_trades;
  size_t winning_trades;
  size_t losing_trades;
  double initial_capital;
  double final_capital;
  double total_pnl;
  double total_fees;
  double net_pnl;
  double gross_profit;
  double gross_loss;
  double max_drawdown;
  double max_drawdown_pct;
  double win_rate;
  double profit_factor;
  double avg_win;
  double avg_loss;
  double sharpe;
  double sortino;
  double calmar;
  double return_pct;

  static PyStats fromBacktest(const BacktestStats& s)
  {
    return {.total_trades = s.totalTrades,
            .winning_trades = s.winningTrades,
            .losing_trades = s.losingTrades,
            .initial_capital = s.initialCapital,
            .final_capital = s.finalCapital,
            .total_pnl = s.totalPnl,
            .total_fees = s.totalFees,
            .net_pnl = s.netPnl,
            .gross_profit = s.grossProfit,
            .gross_loss = s.grossLoss,
            .max_drawdown = s.maxDrawdown,
            .max_drawdown_pct = s.maxDrawdownPct,
            .win_rate = s.winRate,
            .profit_factor = s.profitFactor,
            .avg_win = s.avgWin,
            .avg_loss = s.avgLoss,
            .sharpe = s.sharpeRatio,
            .sortino = s.sortinoRatio,
            .calmar = s.calmarRatio,
            .return_pct = s.returnPct};
  }

  py::dict toDict() const
  {
    py::dict d;
    d["total_trades"] = total_trades;
    d["winning_trades"] = winning_trades;
    d["losing_trades"] = losing_trades;
    d["initial_capital"] = initial_capital;
    d["final_capital"] = final_capital;
    d["total_pnl"] = total_pnl;
    d["total_fees"] = total_fees;
    d["net_pnl"] = net_pnl;
    d["gross_profit"] = gross_profit;
    d["gross_loss"] = gross_loss;
    d["max_drawdown"] = max_drawdown;
    d["max_drawdown_pct"] = max_drawdown_pct;
    d["win_rate"] = win_rate;
    d["profit_factor"] = profit_factor;
    d["avg_win"] = avg_win;
    d["avg_loss"] = avg_loss;
    d["sharpe"] = sharpe;
    d["sortino"] = sortino;
    d["calmar"] = calmar;
    d["return_pct"] = return_pct;
    return d;
  }

  std::string repr() const
  {
    std::ostringstream os;
    os << "Stats(trades=" << total_trades << " pnl=" << net_pnl << " ret=" << return_pct
       << "% sharpe=" << sharpe << " dd=" << max_drawdown_pct << "%)";
    return os.str();
  }
};

// ── Backtest execution ──────────────────────────────────────────────

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

// ── Engine ──────────────────────────────────────────────────────────

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

  void loadBarsFromArrays(contiguous_i64 timestamps, contiguous_f64 open, contiguous_f64 high,
                          contiguous_f64 low, contiguous_f64 close, contiguous_f64 volume)
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
      _bars[i] = {.timestamp_ns = normalizeTimestamp(ts(i)),
                  .open_raw = Price::fromDouble(o(i)).raw(),
                  .high_raw = Price::fromDouble(h(i)).raw(),
                  .low_raw = Price::fromDouble(l(i)).raw(),
                  .close_raw = Price::fromDouble(c(i)).raw(),
                  .volume_raw = Volume::fromDouble(v(i)).raw()};
    }
  }

  void loadBarsFromDict(py::dict d)
  {
    auto get = [&](const char* key) -> contiguous_f64
    {
      if (!d.contains(key))
      {
        throw std::invalid_argument(std::string("missing key: ") + key);
      }
      return d[key].cast<contiguous_f64>();
    };

    contiguous_i64 timestamps;
    if (d.contains("ts"))
    {
      timestamps = d["ts"].cast<contiguous_i64>();
    }
    else if (d.contains("timestamp"))
    {
      timestamps = d["timestamp"].cast<contiguous_i64>();
    }
    else
    {
      throw std::invalid_argument("missing key: ts or timestamp");
    }

    loadBarsFromArrays(timestamps, get("open"), get("high"), get("low"), get("close"),
                       get("volume"));
  }

  void loadCsv(const std::string& path)
  {
    std::ifstream file(path);
    if (!file.is_open())
    {
      throw std::runtime_error("cannot open: " + path);
    }

    std::string line;
    std::getline(file, line);  // skip header

    std::vector<int64_t> ts;
    std::vector<double> op, hi, lo, cl, vol;

    while (std::getline(file, line))
    {
      if (line.empty())
      {
        continue;
      }
      std::istringstream ss(line);
      std::string tok;

      std::getline(ss, tok, ',');
      ts.push_back(std::stoll(tok));
      std::getline(ss, tok, ',');
      op.push_back(std::stod(tok));
      std::getline(ss, tok, ',');
      hi.push_back(std::stod(tok));
      std::getline(ss, tok, ',');
      lo.push_back(std::stod(tok));
      std::getline(ss, tok, ',');
      cl.push_back(std::stod(tok));
      std::getline(ss, tok, ',');
      vol.push_back(std::stod(tok));
    }

    size_t n = ts.size();
    _bars.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
      _bars[i] = {.timestamp_ns = normalizeTimestamp(ts[i]),
                  .open_raw = Price::fromDouble(op[i]).raw(),
                  .high_raw = Price::fromDouble(hi[i]).raw(),
                  .low_raw = Price::fromDouble(lo[i]).raw(),
                  .close_raw = Price::fromDouble(cl[i]).raw(),
                  .volume_raw = Volume::fromDouble(vol[i]).raw()};
    }
  }

  void loadDf(py::object df)
  {
    auto cols = df.attr("columns").attr("tolist")().cast<py::list>();
    auto colSet = py::set(cols);

    // Find timestamp column
    contiguous_i64 timestamps;
    for (const char* key : {"ts", "timestamp", "open_time", "time"})
    {
      if (colSet.contains(key))
      {
        timestamps = df.attr("__getitem__")(key).attr("values").cast<contiguous_i64>();
        break;
      }
    }
    if (timestamps.size() == 0)
    {
      timestamps = df.attr("index").attr("values").cast<contiguous_i64>();
    }

    auto col = [&](const char* key) -> contiguous_f64
    { return df.attr("__getitem__")(key).attr("values").cast<contiguous_f64>(); };

    loadBarsFromArrays(timestamps, col("open"), col("high"), col("low"), col("close"),
                       col("volume"));
  }

  PyStats run(py::array_t<PySignal, py::array::c_style> signals, uint32_t symbol = 1)
  {
    auto buf = signals.request();
    const auto* sigs = static_cast<const PySignal*>(buf.ptr);
    size_t n = buf.shape[0];

    BacktestStats stats;
    {
      py::gil_scoped_release release;
      stats = executeSignals(_bars.data(), _bars.size(), sigs, n, symbol, _config);
    }
    return PyStats::fromBacktest(stats);
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
          results[idx] = executeSignals(_bars.data(), _bars.size(), runs[idx].data,
                                        runs[idx].count, symbol, _config);
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
      out.append(PyStats::fromBacktest(results[i]));
    }
    return out;
  }

  size_t barCount() const { return _bars.size(); }

  py::array_t<int64_t> timestamps() const
  {
    size_t n = _bars.size();
    py::array_t<int64_t> out(n);
    auto* p = out.mutable_data();
    for (size_t i = 0; i < n; ++i)
    {
      p[i] = _bars[i].timestamp_ns;
    }
    return out;
  }

  py::array_t<double> extractDoubles(int64_t PyBar::*field) const
  {
    size_t n = _bars.size();
    py::array_t<double> out(n);
    auto* p = out.mutable_data();
    for (size_t i = 0; i < n; ++i)
    {
      p[i] = Price::fromRaw(_bars[i].*field).toDouble();
    }
    return out;
  }

  py::array_t<double> opens() const { return extractDoubles(&PyBar::open_raw); }
  py::array_t<double> highs() const { return extractDoubles(&PyBar::high_raw); }
  py::array_t<double> lows() const { return extractDoubles(&PyBar::low_raw); }
  py::array_t<double> closes() const { return extractDoubles(&PyBar::close_raw); }
  py::array_t<double> volumes() const { return extractDoubles(&PyBar::volume_raw); }

 private:
  BacktestConfig _config;
  std::vector<PyBar> _bars;
};

// ── Module ──────────────────────────────────────────────────────────

PYBIND11_MODULE(flox_py, m)
{
  m.doc() = "Flox -- Python bindings";

  PYBIND11_NUMPY_DTYPE(PySignal, timestamp_ns, quantity_raw, price_raw, side, order_type);
  PYBIND11_NUMPY_DTYPE(PyBar, timestamp_ns, open_raw, high_raw, low_raw, close_raw, volume_raw);

  // Stats
  py::class_<PyStats>(m, "Stats")
      .def_readonly("total_trades", &PyStats::total_trades)
      .def_readonly("winning_trades", &PyStats::winning_trades)
      .def_readonly("losing_trades", &PyStats::losing_trades)
      .def_readonly("initial_capital", &PyStats::initial_capital)
      .def_readonly("final_capital", &PyStats::final_capital)
      .def_readonly("total_pnl", &PyStats::total_pnl)
      .def_readonly("total_fees", &PyStats::total_fees)
      .def_readonly("net_pnl", &PyStats::net_pnl)
      .def_readonly("gross_profit", &PyStats::gross_profit)
      .def_readonly("gross_loss", &PyStats::gross_loss)
      .def_readonly("max_drawdown", &PyStats::max_drawdown)
      .def_readonly("max_drawdown_pct", &PyStats::max_drawdown_pct)
      .def_readonly("win_rate", &PyStats::win_rate)
      .def_readonly("profit_factor", &PyStats::profit_factor)
      .def_readonly("avg_win", &PyStats::avg_win)
      .def_readonly("avg_loss", &PyStats::avg_loss)
      .def_readonly("sharpe", &PyStats::sharpe)
      .def_readonly("sortino", &PyStats::sortino)
      .def_readonly("calmar", &PyStats::calmar)
      .def_readonly("return_pct", &PyStats::return_pct)
      .def("to_dict", &PyStats::toDict)
      .def("__repr__", &PyStats::repr)
      .def("__getitem__",
           [](const PyStats& s, const std::string& key) -> py::object
           { return s.toDict()[py::str(key)]; });

  // SignalBuilder
  py::class_<SignalBuilder>(m, "SignalBuilder")
      .def(py::init<>())
      .def("buy", &SignalBuilder::buy, py::arg("ts"), py::arg("qty"))
      .def("sell", &SignalBuilder::sell, py::arg("ts"), py::arg("qty"))
      .def("limit_buy", &SignalBuilder::limitBuy, py::arg("ts"), py::arg("price"), py::arg("qty"))
      .def("limit_sell", &SignalBuilder::limitSell, py::arg("ts"), py::arg("price"),
           py::arg("qty"))
      .def("build", &SignalBuilder::build)
      .def("clear", &SignalBuilder::clear)
      .def("__len__", &SignalBuilder::size);

  // Engine
  py::class_<Engine>(m, "Engine")
      .def(py::init<double, double>(), py::arg("initial_capital") = 100000.0,
           py::arg("fee_rate") = 0.0001)
      .def("load_bars", &Engine::loadBars, "Load from numpy structured array")
      .def("load_bars_df", &Engine::loadBarsFromArrays, "Load from separate arrays",
           py::arg("timestamps"), py::arg("open"), py::arg("high"), py::arg("low"),
           py::arg("close"), py::arg("volume"))
      .def("load_ohlcv", &Engine::loadBarsFromDict,
           "Load from dict {ts, open, high, low, close, volume}", py::arg("data"))
      .def("load_csv", &Engine::loadCsv, "Load from CSV file (timestamp,open,high,low,close,volume)",
           py::arg("path"))
      .def("load_df", &Engine::loadDf, "Load from pandas DataFrame", py::arg("df"))
      .def("run", &Engine::run, py::arg("signals"), py::arg("symbol") = 1)
      .def("run_batch", &Engine::runBatch, py::arg("signal_sets"), py::arg("threads") = 0,
           py::arg("symbol") = 1)
      .def_property_readonly("bar_count", &Engine::barCount)
      .def_property_readonly("ts", &Engine::timestamps)
      .def_property_readonly("open", &Engine::opens)
      .def_property_readonly("high", &Engine::highs)
      .def_property_readonly("low", &Engine::lows)
      .def_property_readonly("close", &Engine::closes)
      .def_property_readonly("volume", &Engine::volumes);

  // make_signals (keep for backwards compat)
  m.def(
      "make_signals",
      [](py::array_t<int64_t> timestamps, py::array_t<uint8_t> sides,
         py::array_t<double> quantities, std::optional<py::array_t<double>> prices,
         std::optional<py::array_t<uint8_t>> types) -> py::array_t<PySignal>
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
        if (hasPrices)
        {
          px.emplace(prices->unchecked<1>());
        }
        if (hasTypes)
        {
          tp.emplace(types->unchecked<1>());
        }

        for (py::ssize_t i = 0; i < n; ++i)
        {
          buf(i).timestamp_ns = normalizeTimestamp(ts(i));
          buf(i).quantity_raw = Quantity::fromDouble(qt(i)).raw();
          buf(i).price_raw = px ? Price::fromDouble((*px)(i)).raw() : 0;
          buf(i).side = sd(i);
          buf(i).order_type = tp ? (*tp)(i) : 0;
        }
        return result;
      },
      py::arg("timestamps"), py::arg("sides"), py::arg("quantities"),
      py::arg("prices") = py::none(), py::arg("types") = py::none());

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
