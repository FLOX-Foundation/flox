// python/backtest_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/common.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

using namespace flox;

#pragma pack(push, 1)
struct PyFill
{
  uint64_t order_id;
  uint32_t symbol;
  uint8_t side;
  uint8_t _pad[3];
  int64_t price_raw;
  int64_t quantity_raw;
  int64_t timestamp_ns;
};
#pragma pack(pop)
static_assert(sizeof(PyFill) == 40);

#pragma pack(push, 1)
struct PyTradeRecord
{
  uint32_t symbol;
  uint8_t side;
  uint8_t _pad[3];
  int64_t entry_price_raw;
  int64_t exit_price_raw;
  int64_t quantity_raw;
  int64_t entry_time_ns;
  int64_t exit_time_ns;
  int64_t pnl_raw;
  int64_t fee_raw;
};
#pragma pack(pop)
static_assert(sizeof(PyTradeRecord) == 64);

#pragma pack(push, 1)
struct PyEquityPoint
{
  int64_t timestamp_ns;
  double equity;
  double drawdown_pct;
};
#pragma pack(pop)
static_assert(sizeof(PyEquityPoint) == 24);

inline PyFill fillToPyFill(const Fill& f)
{
  return {.order_id = f.orderId,
          .symbol = f.symbol,
          .side = static_cast<uint8_t>(f.side == Side::BUY ? 0 : 1),
          ._pad = {},
          .price_raw = f.price.raw(),
          .quantity_raw = f.quantity.raw(),
          .timestamp_ns = static_cast<int64_t>(f.timestampNs)};
}

inline PyTradeRecord tradeRecToPy(const TradeRecord& t)
{
  return {.symbol = t.symbol,
          .side = static_cast<uint8_t>(t.side == Side::BUY ? 0 : 1),
          ._pad = {},
          .entry_price_raw = t.entryPrice.raw(),
          .exit_price_raw = t.exitPrice.raw(),
          .quantity_raw = t.quantity.raw(),
          .entry_time_ns = static_cast<int64_t>(t.entryTimeNs),
          .exit_time_ns = static_cast<int64_t>(t.exitTimeNs),
          .pnl_raw = t.pnl.raw(),
          .fee_raw = t.fee.raw()};
}

inline PyEquityPoint equityToPy(const EquityPoint& p)
{
  return {.timestamp_ns = static_cast<int64_t>(p.timestampNs),
          .equity = p.equity,
          .drawdown_pct = p.drawdownPct};
}

inline SlippageProfile makePyProfile(const std::string& model, int32_t ticks, double bps,
                                     double impact)
{
  SlippageProfile p;
  if (model == "fixed_ticks")
  {
    p.model = SlippageModel::FIXED_TICKS;
  }
  else if (model == "fixed_bps")
  {
    p.model = SlippageModel::FIXED_BPS;
  }
  else if (model == "volume_impact")
  {
    p.model = SlippageModel::VOLUME_IMPACT;
  }
  else
  {
    p.model = SlippageModel::NONE;
  }
  p.ticks = ticks;
  p.bps = bps;
  p.impactCoeff = impact;
  return p;
}

// Wraps SimulatedExecutor + SimulatedClock for standalone use
class PySimulatedExecutor
{
 public:
  PySimulatedExecutor() : _executor(_clock) { _executor.start(); }

  void submitOrder(uint64_t id, const std::string& sideStr, double price, double qty,
                   const std::string& typeStr, uint32_t symbol)
  {
    Order order;
    order.id = id;
    order.side = (sideStr == "buy") ? Side::BUY : Side::SELL;
    order.price = Price::fromDouble(price);
    order.quantity = Quantity::fromDouble(qty);
    order.symbol = symbol;
    order.type = parseOrderType(typeStr);
    _executor.submitOrder(order);
  }

  void cancelOrder(uint64_t id) { _executor.cancelOrder(id); }
  void cancelAll(uint32_t symbol) { _executor.cancelAllOrders(symbol); }

  void onBar(uint32_t symbol, double closePrice)
  {
    _executor.onBar(symbol, Price::fromDouble(closePrice));
  }

  void onTrade(uint32_t symbol, double price, bool isBuy)
  {
    _executor.onTrade(symbol, Price::fromDouble(price), isBuy);
  }

  void onTradeQty(uint32_t symbol, double price, double qty, bool isBuy)
  {
    _executor.onTrade(symbol, Price::fromDouble(price), Quantity::fromDouble(qty), isBuy);
  }

  void onBookLevel(uint32_t symbol, const std::string& sideStr, double price, double qty)
  {
    std::pmr::monotonic_buffer_resource pool(512);
    std::pmr::vector<BookLevel> bids(&pool);
    std::pmr::vector<BookLevel> asks(&pool);
    BookLevel lvl(Price::fromDouble(price), Quantity::fromDouble(qty));
    if (sideStr == "bid")
    {
      bids.push_back(lvl);
    }
    else
    {
      asks.push_back(lvl);
    }
    _executor.onBookUpdate(symbol, bids, asks);
  }

  void advanceClock(int64_t timestampNs) { _clock.advanceTo(timestampNs); }

  void setDefaultSlippage(const std::string& model, int32_t ticks, double bps, double impact)
  {
    _executor.setDefaultSlippage(makePyProfile(model, ticks, bps, impact));
  }

  void setSymbolSlippage(uint32_t symbol, const std::string& model, int32_t ticks, double bps,
                         double impact)
  {
    _executor.setSymbolSlippage(symbol, makePyProfile(model, ticks, bps, impact));
  }

  void setQueueModel(const std::string& model, uint32_t depth)
  {
    QueueModel qm = QueueModel::NONE;
    if (model == "tob")
    {
      qm = QueueModel::TOB;
    }
    else if (model == "full")
    {
      qm = QueueModel::FULL;
    }
    _executor.setQueueModel(qm, depth);
  }

  py::array_t<PyFill> fills() const
  {
    const auto& f = _executor.fills();
    py::array_t<PyFill> result(f.size());
    auto* out = result.mutable_data();
    for (size_t i = 0; i < f.size(); ++i)
    {
      out[i] = fillToPyFill(f[i]);
    }
    return result;
  }

  py::list fillsList() const
  {
    py::list result;
    for (const auto& f : _executor.fills())
    {
      py::dict d;
      d["order_id"] = f.orderId;
      d["symbol"] = f.symbol;
      d["side"] = (f.side == Side::BUY) ? "buy" : "sell";
      d["price"] = f.price.toDouble();
      d["quantity"] = f.quantity.toDouble();
      d["timestamp_ns"] = static_cast<int64_t>(f.timestampNs);
      result.append(d);
    }
    return result;
  }

  size_t fillCount() const { return _executor.fills().size(); }

  const std::vector<Fill>& rawFills() const { return _executor.fills(); }

 private:
  static OrderType parseOrderType(const std::string& s)
  {
    if (s == "limit")
    {
      return OrderType::LIMIT;
    }
    if (s == "stop_market")
    {
      return OrderType::STOP_MARKET;
    }
    if (s == "stop_limit")
    {
      return OrderType::STOP_LIMIT;
    }
    if (s == "take_profit_market")
    {
      return OrderType::TAKE_PROFIT_MARKET;
    }
    if (s == "take_profit_limit")
    {
      return OrderType::TAKE_PROFIT_LIMIT;
    }
    if (s == "trailing_stop")
    {
      return OrderType::TRAILING_STOP;
    }
    return OrderType::MARKET;
  }

  SimulatedClock _clock;
  SimulatedExecutor _executor;
};

class PyBacktestResult
{
 public:
  PyBacktestResult(double initialCapital, double feeRate, bool usePercentageFee,
                   double fixedFeePerTrade, double riskFreeRate, double annualization)
  {
    _config.initialCapital = initialCapital;
    _config.feeRate = feeRate;
    _config.usePercentageFee = usePercentageFee;
    _config.fixedFeePerTrade = fixedFeePerTrade;
    _config.riskFreeRate = riskFreeRate;
    _config.metricsAnnualizationFactor = (annualization > 0.0) ? annualization : 252.0;
    _result = std::make_unique<BacktestResult>(_config);
  }

  void recordFill(uint64_t orderId, uint32_t symbol, const std::string& sideStr, double price,
                  double qty, int64_t timestampNs)
  {
    Fill fill{};
    fill.orderId = orderId;
    fill.symbol = symbol;
    fill.side = (sideStr == "buy") ? Side::BUY : Side::SELL;
    fill.price = Price::fromDouble(price);
    fill.quantity = Quantity::fromDouble(qty);
    fill.timestampNs = static_cast<UnixNanos>(timestampNs);
    _result->recordFill(fill);
  }

  void ingestExecutor(const PySimulatedExecutor& exec)
  {
    for (const auto& fill : exec.rawFills())
    {
      _result->recordFill(fill);
    }
  }

  py::dict stats() const
  {
    auto s = _result->computeStats();
    py::dict d;
    d["total_trades"] = s.totalTrades;
    d["winning_trades"] = s.winningTrades;
    d["losing_trades"] = s.losingTrades;
    d["max_consecutive_wins"] = s.maxConsecutiveWins;
    d["max_consecutive_losses"] = s.maxConsecutiveLosses;
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
    d["avg_win_loss_ratio"] = s.avgWinLossRatio;
    d["avg_trade_duration_ns"] = s.avgTradeDurationNs;
    d["median_trade_duration_ns"] = s.medianTradeDurationNs;
    d["max_trade_duration_ns"] = s.maxTradeDurationNs;
    d["sharpe_ratio"] = s.sharpeRatio;
    d["sortino_ratio"] = s.sortinoRatio;
    d["calmar_ratio"] = s.calmarRatio;
    d["time_weighted_return"] = s.timeWeightedReturn;
    d["return_pct"] = s.returnPct;
    d["start_time_ns"] = static_cast<int64_t>(s.startTimeNs);
    d["end_time_ns"] = static_cast<int64_t>(s.endTimeNs);
    return d;
  }

  py::array_t<PyTradeRecord> trades() const
  {
    const auto& t = _result->trades();
    py::array_t<PyTradeRecord> arr(t.size());
    auto* out = arr.mutable_data();
    for (size_t i = 0; i < t.size(); ++i)
    {
      out[i] = tradeRecToPy(t[i]);
    }
    return arr;
  }

  py::array_t<PyEquityPoint> equityCurve() const
  {
    const auto& c = _result->equityCurve();
    py::array_t<PyEquityPoint> arr(c.size());
    auto* out = arr.mutable_data();
    for (size_t i = 0; i < c.size(); ++i)
    {
      out[i] = equityToPy(c[i]);
    }
    return arr;
  }

  bool writeEquityCurveCsv(const std::string& path) const
  {
    return _result->writeEquityCurveCsv(path);
  }

 private:
  BacktestConfig _config;
  std::unique_ptr<BacktestResult> _result;
};

}  // namespace

inline void bindBacktest(py::module_& m)
{
  PYBIND11_NUMPY_DTYPE(PyFill, order_id, symbol, side, price_raw, quantity_raw, timestamp_ns);
  PYBIND11_NUMPY_DTYPE(PyTradeRecord, symbol, side, entry_price_raw, exit_price_raw,
                       quantity_raw, entry_time_ns, exit_time_ns, pnl_raw, fee_raw);
  PYBIND11_NUMPY_DTYPE(PyEquityPoint, timestamp_ns, equity, drawdown_pct);

  py::class_<PySimulatedExecutor>(m, "SimulatedExecutor")
      .def(py::init<>())
      .def("submit_order", &PySimulatedExecutor::submitOrder,
           "Submit an order to the simulated exchange",
           py::arg("id"), py::arg("side"), py::arg("price"), py::arg("quantity"),
           py::arg("type") = "market", py::arg("symbol") = 1)
      .def("cancel_order", &PySimulatedExecutor::cancelOrder, py::arg("order_id"))
      .def("cancel_all", &PySimulatedExecutor::cancelAll, py::arg("symbol"))
      .def("on_bar", &PySimulatedExecutor::onBar,
           "Feed a bar close price for order matching",
           py::arg("symbol"), py::arg("close_price"))
      .def("on_trade", &PySimulatedExecutor::onTrade,
           "Feed a trade for order matching",
           py::arg("symbol"), py::arg("price"), py::arg("is_buy"))
      .def("on_trade_qty", &PySimulatedExecutor::onTradeQty,
           "Feed a trade with quantity (enables queue-fill simulation)",
           py::arg("symbol"), py::arg("price"), py::arg("quantity"), py::arg("is_buy"))
      .def("on_book_level", &PySimulatedExecutor::onBookLevel,
           "Feed a single L2 level update",
           py::arg("symbol"), py::arg("side"), py::arg("price"), py::arg("quantity"))
      .def("advance_clock", &PySimulatedExecutor::advanceClock,
           "Advance simulation clock to timestamp",
           py::arg("timestamp_ns"))
      .def("set_default_slippage", &PySimulatedExecutor::setDefaultSlippage,
           "Configure default slippage. model: none|fixed_ticks|fixed_bps|volume_impact",
           py::arg("model"), py::arg("ticks") = 0, py::arg("bps") = 0.0,
           py::arg("impact_coeff") = 0.0)
      .def("set_symbol_slippage", &PySimulatedExecutor::setSymbolSlippage,
           "Configure slippage for a specific symbol",
           py::arg("symbol"), py::arg("model"), py::arg("ticks") = 0,
           py::arg("bps") = 0.0, py::arg("impact_coeff") = 0.0)
      .def("set_queue_model", &PySimulatedExecutor::setQueueModel,
           "Configure queue simulation. model: none|tob|full",
           py::arg("model"), py::arg("depth") = 1)
      .def("fills", &PySimulatedExecutor::fills,
           "Get all fills as numpy structured array")
      .def("fills_list", &PySimulatedExecutor::fillsList,
           "Get all fills as list of dicts")
      .def_property_readonly("fill_count", &PySimulatedExecutor::fillCount);

  py::class_<PyBacktestResult>(m, "BacktestResult")
      .def(py::init<double, double, bool, double, double, double>(),
           py::arg("initial_capital") = 100000.0, py::arg("fee_rate") = 0.0001,
           py::arg("use_percentage_fee") = true, py::arg("fixed_fee_per_trade") = 0.0,
           py::arg("risk_free_rate") = 0.0, py::arg("annualization_factor") = 252.0)
      .def("record_fill", &PyBacktestResult::recordFill,
           py::arg("order_id"), py::arg("symbol"), py::arg("side"), py::arg("price"),
           py::arg("quantity"), py::arg("timestamp_ns"))
      .def("ingest_executor", &PyBacktestResult::ingestExecutor, py::arg("executor"),
           "Drain executor fills into this result in FIFO order")
      .def("stats", &PyBacktestResult::stats,
           "Compute stats as a dict (includes new metrics: streaks, durations, TWR)")
      .def("trades", &PyBacktestResult::trades,
           "Return trade records as numpy structured array")
      .def("equity_curve", &PyBacktestResult::equityCurve,
           "Return equity curve as numpy structured array")
      .def("write_equity_curve_csv", &PyBacktestResult::writeEquityCurveCsv, py::arg("path"),
           "Write equity curve to CSV (timestamp_ns,equity,drawdown_pct header)");
}
