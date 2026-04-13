// python/backtest_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/backtest/backtest_result.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/common.h"

#include <cstring>
#include <memory>
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

  void advanceClock(int64_t timestampNs) { _clock.advanceTo(timestampNs); }

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

}  // namespace

inline void bindBacktest(py::module_& m)
{
  PYBIND11_NUMPY_DTYPE(PyFill, order_id, symbol, side, price_raw, quantity_raw, timestamp_ns);
  PYBIND11_NUMPY_DTYPE(PyTradeRecord, symbol, side, entry_price_raw, exit_price_raw,
                       quantity_raw, entry_time_ns, exit_time_ns, pnl_raw, fee_raw);

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
      .def("advance_clock", &PySimulatedExecutor::advanceClock,
           "Advance simulation clock to timestamp",
           py::arg("timestamp_ns"))
      .def("fills", &PySimulatedExecutor::fills,
           "Get all fills as numpy structured array")
      .def("fills_list", &PySimulatedExecutor::fillsList,
           "Get all fills as list of dicts")
      .def_property_readonly("fill_count", &PySimulatedExecutor::fillCount);
}
