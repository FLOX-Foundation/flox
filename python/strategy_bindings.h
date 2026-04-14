// python/strategy_bindings.h

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/capi/bridge_strategy.h"

#include <optional>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

using namespace flox;

struct PyTradeData
{
  uint32_t symbol;
  double price;
  double quantity;
  bool is_buy;
  int64_t timestamp_ns;
};

struct PySymbolCtx
{
  uint32_t symbol_id;
  double position;
  double last_trade_price;
  double best_bid;
  double best_ask;
  double mid_price;
  double unrealized_pnl;

  double book_spread() const
  {
    if (best_bid > 0 && best_ask > 0)
    {
      return best_ask - best_bid;
    }
    return 0.0;
  }

  bool is_long() const { return position > 0; }
  bool is_short() const { return position < 0; }
  bool is_flat() const { return position == 0; }
};

inline Side parseSide(const std::string& s)
{
  return (s == "buy") ? Side::BUY : Side::SELL;
}

class PyStrategyBase
{
 public:
  PyStrategyBase(std::vector<uint32_t> symbols) : _symbols(std::move(symbols)) {}

  virtual ~PyStrategyBase() = default;

  virtual void on_trade(const PySymbolCtx& ctx, const PyTradeData& trade) {}
  virtual void on_book_update(const PySymbolCtx& ctx) {}
  virtual void on_start() {}
  virtual void on_stop() {}

  uint64_t emit_market_buy(uint32_t symbol, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitMarketBuy(symbol, Quantity::fromDouble(qty));
  }

  uint64_t emit_market_sell(uint32_t symbol, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitMarketSell(symbol, Quantity::fromDouble(qty));
  }

  uint64_t emit_limit_buy(uint32_t symbol, double price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitLimitBuy(symbol, Price::fromDouble(price),
                                       Quantity::fromDouble(qty));
  }

  uint64_t emit_limit_sell(uint32_t symbol, double price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitLimitSell(symbol, Price::fromDouble(price),
                                        Quantity::fromDouble(qty));
  }

  void emit_cancel(uint64_t order_id)
  {
    if (_bridge)
    {
      _bridge->publicEmitCancel(order_id);
    }
  }

  void emit_cancel_all(uint32_t symbol)
  {
    if (_bridge)
    {
      _bridge->publicEmitCancelAll(symbol);
    }
  }

  void emit_modify(uint64_t order_id, double new_price, double new_qty)
  {
    if (_bridge)
    {
      _bridge->publicEmitModify(order_id, Price::fromDouble(new_price),
                                Quantity::fromDouble(new_qty));
    }
  }

  uint64_t emit_stop_market(uint32_t symbol, const std::string& side, double trigger, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitStopMarket(symbol, parseSide(side), Price::fromDouble(trigger),
                                         Quantity::fromDouble(qty));
  }

  uint64_t emit_stop_limit(uint32_t symbol, const std::string& side, double trigger,
                           double limit_price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitStopLimit(symbol, parseSide(side), Price::fromDouble(trigger),
                                        Price::fromDouble(limit_price),
                                        Quantity::fromDouble(qty));
  }

  uint64_t emit_take_profit_market(uint32_t symbol, const std::string& side, double trigger,
                                   double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitTakeProfitMarket(symbol, parseSide(side),
                                               Price::fromDouble(trigger),
                                               Quantity::fromDouble(qty));
  }

  uint64_t emit_take_profit_limit(uint32_t symbol, const std::string& side, double trigger,
                                  double limit_price, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitTakeProfitLimit(symbol, parseSide(side),
                                              Price::fromDouble(trigger),
                                              Price::fromDouble(limit_price),
                                              Quantity::fromDouble(qty));
  }

  uint64_t emit_trailing_stop(uint32_t symbol, const std::string& side, double offset, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitTrailingStop(symbol, parseSide(side), Price::fromDouble(offset),
                                           Quantity::fromDouble(qty));
  }

  uint64_t emit_trailing_stop_percent(uint32_t symbol, const std::string& side,
                                      int32_t callback_bps, double qty)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitTrailingStopPercent(symbol, parseSide(side), callback_bps,
                                                  Quantity::fromDouble(qty));
  }

  uint64_t emit_limit_buy_tif(uint32_t symbol, double price, double qty, const std::string& tif)
  {
    if (!_bridge)
    {
      return 0;
    }
    TimeInForce t = TimeInForce::GTC;
    if (tif == "ioc")
    {
      t = TimeInForce::IOC;
    }
    else if (tif == "fok")
    {
      t = TimeInForce::FOK;
    }
    else if (tif == "post_only")
    {
      t = TimeInForce::POST_ONLY;
    }
    return _bridge->publicEmitLimitBuyTif(symbol, Price::fromDouble(price),
                                          Quantity::fromDouble(qty), t);
  }

  uint64_t emit_limit_sell_tif(uint32_t symbol, double price, double qty, const std::string& tif)
  {
    if (!_bridge)
    {
      return 0;
    }
    TimeInForce t = TimeInForce::GTC;
    if (tif == "ioc")
    {
      t = TimeInForce::IOC;
    }
    else if (tif == "fok")
    {
      t = TimeInForce::FOK;
    }
    else if (tif == "post_only")
    {
      t = TimeInForce::POST_ONLY;
    }
    return _bridge->publicEmitLimitSellTif(symbol, Price::fromDouble(price),
                                           Quantity::fromDouble(qty), t);
  }

  uint64_t emit_close_position(uint32_t symbol)
  {
    if (!_bridge)
    {
      return 0;
    }
    return _bridge->publicEmitClosePosition(symbol);
  }

  int32_t get_order_status(uint64_t order_id) const
  {
    if (!_bridge)
    {
      return -1;
    }
    auto status = _bridge->getOrderStatus(order_id);
    return status ? static_cast<int32_t>(*status) : -1;
  }

  double position(std::optional<uint32_t> symbol = std::nullopt) const
  {
    if (!_bridge)
    {
      return 0.0;
    }
    return _bridge->position(symbol.value_or(_symbols[0])).toDouble();
  }

  PySymbolCtx ctx(std::optional<uint32_t> symbol = std::nullopt) const
  {
    PySymbolCtx result{};
    if (!_bridge)
    {
      return result;
    }
    const auto& c = _bridge->ctx(symbol.value_or(_symbols[0]));
    result.symbol_id = c.symbolId;
    result.position = c.position.toDouble();
    result.last_trade_price = c.lastTradePrice.toDouble();
    auto bid = c.book.bestBid();
    result.best_bid = bid ? bid->toDouble() : 0.0;
    auto ask = c.book.bestAsk();
    result.best_ask = ask ? ask->toDouble() : 0.0;
    auto mid = c.mid();
    result.mid_price = mid ? mid->toDouble() : 0.0;
    result.unrealized_pnl = c.unrealizedPnl();
    return result;
  }

  const std::vector<uint32_t>& symbols() const { return _symbols; }

  // Internal: set up bridge for backtest
  void _initBridge(BridgeStrategy* bridge) { _bridge = bridge; }

 private:
  std::vector<uint32_t> _symbols;
  BridgeStrategy* _bridge = nullptr;
};

class PyStrategyTrampoline : public PyStrategyBase
{
 public:
  using PyStrategyBase::PyStrategyBase;

  void on_trade(const PySymbolCtx& ctx, const PyTradeData& trade) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_trade, ctx, trade);
  }

  void on_book_update(const PySymbolCtx& ctx) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_book_update, ctx);
  }

  void on_start() override { PYBIND11_OVERRIDE(void, PyStrategyBase, on_start); }

  void on_stop() override { PYBIND11_OVERRIDE(void, PyStrategyBase, on_stop); }
};

}  // namespace

inline void bindStrategy(py::module_& m)
{
  py::class_<PyTradeData>(m, "TradeData")
      .def(py::init<>())
      .def_readwrite("symbol", &PyTradeData::symbol)
      .def_readwrite("price", &PyTradeData::price)
      .def_readwrite("quantity", &PyTradeData::quantity)
      .def_readwrite("is_buy", &PyTradeData::is_buy)
      .def_readwrite("timestamp_ns", &PyTradeData::timestamp_ns);

  py::class_<PySymbolCtx>(m, "SymbolContext")
      .def(py::init<>())
      .def_readwrite("symbol_id", &PySymbolCtx::symbol_id)
      .def_readwrite("position", &PySymbolCtx::position)
      .def_readwrite("last_trade_price", &PySymbolCtx::last_trade_price)
      .def_readwrite("best_bid", &PySymbolCtx::best_bid)
      .def_readwrite("best_ask", &PySymbolCtx::best_ask)
      .def_readwrite("mid_price", &PySymbolCtx::mid_price)
      .def_readwrite("unrealized_pnl", &PySymbolCtx::unrealized_pnl)
      .def("book_spread", &PySymbolCtx::book_spread)
      .def("is_long", &PySymbolCtx::is_long)
      .def("is_short", &PySymbolCtx::is_short)
      .def("is_flat", &PySymbolCtx::is_flat);

  py::class_<PyStrategyBase, PyStrategyTrampoline>(m, "Strategy")
      .def(py::init<std::vector<uint32_t>>(), py::arg("symbols"))
      .def("on_trade", &PyStrategyBase::on_trade, py::arg("ctx"), py::arg("trade"))
      .def("on_book_update", &PyStrategyBase::on_book_update, py::arg("ctx"))
      .def("on_start", &PyStrategyBase::on_start)
      .def("on_stop", &PyStrategyBase::on_stop)
      .def("emit_market_buy", &PyStrategyBase::emit_market_buy, py::arg("symbol"),
           py::arg("quantity"))
      .def("emit_market_sell", &PyStrategyBase::emit_market_sell, py::arg("symbol"),
           py::arg("quantity"))
      .def("emit_limit_buy", &PyStrategyBase::emit_limit_buy, py::arg("symbol"), py::arg("price"),
           py::arg("quantity"))
      .def("emit_limit_sell", &PyStrategyBase::emit_limit_sell, py::arg("symbol"),
           py::arg("price"), py::arg("quantity"))
      .def("emit_cancel", &PyStrategyBase::emit_cancel, py::arg("order_id"))
      .def("emit_cancel_all", &PyStrategyBase::emit_cancel_all, py::arg("symbol"))
      .def("emit_modify", &PyStrategyBase::emit_modify, py::arg("order_id"),
           py::arg("new_price"), py::arg("new_quantity"))
      .def("emit_stop_market", &PyStrategyBase::emit_stop_market, py::arg("symbol"),
           py::arg("side"), py::arg("trigger"), py::arg("quantity"))
      .def("emit_stop_limit", &PyStrategyBase::emit_stop_limit, py::arg("symbol"),
           py::arg("side"), py::arg("trigger"), py::arg("limit_price"), py::arg("quantity"))
      .def("emit_take_profit_market", &PyStrategyBase::emit_take_profit_market,
           py::arg("symbol"), py::arg("side"), py::arg("trigger"), py::arg("quantity"))
      .def("emit_take_profit_limit", &PyStrategyBase::emit_take_profit_limit, py::arg("symbol"),
           py::arg("side"), py::arg("trigger"), py::arg("limit_price"), py::arg("quantity"))
      .def("emit_trailing_stop", &PyStrategyBase::emit_trailing_stop, py::arg("symbol"),
           py::arg("side"), py::arg("offset"), py::arg("quantity"))
      .def("emit_trailing_stop_percent", &PyStrategyBase::emit_trailing_stop_percent,
           py::arg("symbol"), py::arg("side"), py::arg("callback_bps"), py::arg("quantity"))
      .def("emit_limit_buy_tif", &PyStrategyBase::emit_limit_buy_tif, py::arg("symbol"),
           py::arg("price"), py::arg("quantity"), py::arg("tif") = "gtc")
      .def("emit_limit_sell_tif", &PyStrategyBase::emit_limit_sell_tif, py::arg("symbol"),
           py::arg("price"), py::arg("quantity"), py::arg("tif") = "gtc")
      .def("emit_close_position", &PyStrategyBase::emit_close_position, py::arg("symbol"))
      .def("get_order_status", &PyStrategyBase::get_order_status, py::arg("order_id"))
      .def("position", &PyStrategyBase::position, py::arg("symbol") = py::none())
      .def("ctx", &PyStrategyBase::ctx, py::arg("symbol") = py::none())
      .def_property_readonly("symbols", &PyStrategyBase::symbols);
}
