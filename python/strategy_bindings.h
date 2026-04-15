// python/strategy_bindings.h

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/capi/bridge_strategy.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace py = pybind11;

namespace
{

using namespace flox;

struct PyTradeData
{
  uint32_t symbol;
  std::string symbol_name;
  double price;
  double quantity;
  bool is_buy;
  std::string side;
  int64_t timestamp_ns;
};

struct PySymbolCtx
{
  uint32_t symbol_id;
  std::string symbol;
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

  const std::vector<std::string>& symbol_names() const { return _symbolNames; }

  // Internal: set up bridge for backtest
  void _initBridge(BridgeStrategy* bridge)
  {
    _bridge = bridge;
    _buildSymbolMap();
  }

  // ------------------------------------------------------------------
  // Ergonomic API: string symbols, kwargs, no emit_ prefix
  // ------------------------------------------------------------------

  uint64_t market_buy(double qty, std::optional<std::string> symbol = std::nullopt)
  {
    return emit_market_buy(_resolve(symbol), qty);
  }

  uint64_t market_sell(double qty, std::optional<std::string> symbol = std::nullopt)
  {
    return emit_market_sell(_resolve(symbol), qty);
  }

  uint64_t limit_buy(double price, double qty, std::optional<std::string> symbol = std::nullopt,
                     const std::string& tif = "gtc")
  {
    return emit_limit_buy_tif(_resolve(symbol), price, qty, tif);
  }

  uint64_t limit_sell(double price, double qty, std::optional<std::string> symbol = std::nullopt,
                      const std::string& tif = "gtc")
  {
    return emit_limit_sell_tif(_resolve(symbol), price, qty, tif);
  }

  uint64_t stop_market(const std::string& side, double trigger, double qty,
                       std::optional<std::string> symbol = std::nullopt)
  {
    return emit_stop_market(_resolve(symbol), side, trigger, qty);
  }

  uint64_t stop_limit(const std::string& side, double trigger, double limit_price, double qty,
                      std::optional<std::string> symbol = std::nullopt)
  {
    return emit_stop_limit(_resolve(symbol), side, trigger, limit_price, qty);
  }

  uint64_t take_profit_market(const std::string& side, double trigger, double qty,
                              std::optional<std::string> symbol = std::nullopt)
  {
    return emit_take_profit_market(_resolve(symbol), side, trigger, qty);
  }

  uint64_t take_profit_limit(const std::string& side, double trigger, double limit_price,
                             double qty, std::optional<std::string> symbol = std::nullopt)
  {
    return emit_take_profit_limit(_resolve(symbol), side, trigger, limit_price, qty);
  }

  uint64_t trailing_stop(const std::string& side, double offset, double qty,
                         std::optional<std::string> symbol = std::nullopt)
  {
    return emit_trailing_stop(_resolve(symbol), side, offset, qty);
  }

  uint64_t trailing_stop_percent(const std::string& side, int32_t callback_bps, double qty,
                                 std::optional<std::string> symbol = std::nullopt)
  {
    return emit_trailing_stop_percent(_resolve(symbol), side, callback_bps, qty);
  }

  uint64_t close_position(std::optional<std::string> symbol = std::nullopt)
  {
    return emit_close_position(_resolve(symbol));
  }

  void cancel_order(uint64_t order_id) { emit_cancel(order_id); }

  void cancel_all_orders(std::optional<std::string> symbol = std::nullopt)
  {
    emit_cancel_all(_resolve(symbol));
  }

  void modify_order(uint64_t order_id, double new_price, double new_qty)
  {
    emit_modify(order_id, new_price, new_qty);
  }

  double pos(std::optional<std::string> symbol = std::nullopt) const
  {
    return position(_resolve(symbol));
  }

  int32_t order_status(uint64_t order_id) const { return get_order_status(order_id); }

  std::string primary_symbol_name() const
  {
    return _symbolNames.empty() ? "" : _symbolNames[0];
  }

 private:
  std::vector<uint32_t> _symbols;
  std::vector<std::string> _symbolNames;
  std::unordered_map<std::string, uint32_t> _symbolMap;
  std::unordered_map<uint32_t, std::string> _reverseMap;
  BridgeStrategy* _bridge = nullptr;

  void _buildSymbolMap()
  {
    if (!_bridge)
    {
      return;
    }
    _symbolNames.clear();
    _symbolMap.clear();
    _reverseMap.clear();
    const auto& reg = _bridge->registry();
    for (uint32_t id : _symbols)
    {
      auto [exchange, name] = reg.getSymbolName(id);
      _symbolNames.push_back(name);
      _symbolMap[name] = id;
      _reverseMap[id] = name;
    }
  }

  uint32_t _resolve(std::optional<std::string> symbol) const
  {
    if (!symbol.has_value())
    {
      return _symbols.empty() ? 0 : _symbols[0];
    }
    auto it = _symbolMap.find(symbol.value());
    if (it != _symbolMap.end())
    {
      return it->second;
    }
    throw std::invalid_argument("Unknown symbol: " + symbol.value());
  }

  uint32_t _resolve(std::optional<uint32_t> symbol) const
  {
    return symbol.value_or(_symbols.empty() ? 0 : _symbols[0]);
  }

  std::string _reverseLookup(uint32_t id) const
  {
    auto it = _reverseMap.find(id);
    return it != _reverseMap.end() ? it->second : "";
  }
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
      .def_readwrite("symbol_name", &PyTradeData::symbol_name)
      .def_readwrite("price", &PyTradeData::price)
      .def_readwrite("quantity", &PyTradeData::quantity)
      .def_readwrite("is_buy", &PyTradeData::is_buy)
      .def_readwrite("side", &PyTradeData::side)
      .def_readwrite("timestamp_ns", &PyTradeData::timestamp_ns);

  py::class_<PySymbolCtx>(m, "SymbolContext")
      .def(py::init<>())
      .def_readwrite("symbol_id", &PySymbolCtx::symbol_id)
      .def_readwrite("symbol", &PySymbolCtx::symbol)
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
      .def_property_readonly("symbols", &PyStrategyBase::symbols)
      // Ergonomic API: string symbols, kwargs, no emit_ prefix
      .def("market_buy", &PyStrategyBase::market_buy, py::arg("qty"),
           py::arg("symbol") = py::none())
      .def("market_sell", &PyStrategyBase::market_sell, py::arg("qty"),
           py::arg("symbol") = py::none())
      .def("limit_buy", &PyStrategyBase::limit_buy, py::arg("price"), py::arg("qty"),
           py::arg("symbol") = py::none(), py::arg("tif") = "gtc")
      .def("limit_sell", &PyStrategyBase::limit_sell, py::arg("price"), py::arg("qty"),
           py::arg("symbol") = py::none(), py::arg("tif") = "gtc")
      .def("stop_market", &PyStrategyBase::stop_market, py::arg("side"), py::arg("trigger"),
           py::arg("qty"), py::arg("symbol") = py::none())
      .def("stop_limit", &PyStrategyBase::stop_limit, py::arg("side"), py::arg("trigger"),
           py::arg("limit_price"), py::arg("qty"), py::arg("symbol") = py::none())
      .def("take_profit_market", &PyStrategyBase::take_profit_market, py::arg("side"),
           py::arg("trigger"), py::arg("qty"), py::arg("symbol") = py::none())
      .def("take_profit_limit", &PyStrategyBase::take_profit_limit, py::arg("side"),
           py::arg("trigger"), py::arg("limit_price"), py::arg("qty"),
           py::arg("symbol") = py::none())
      .def("trailing_stop", &PyStrategyBase::trailing_stop, py::arg("side"), py::arg("offset"),
           py::arg("qty"), py::arg("symbol") = py::none())
      .def("trailing_stop_percent", &PyStrategyBase::trailing_stop_percent, py::arg("side"),
           py::arg("callback_bps"), py::arg("qty"), py::arg("symbol") = py::none())
      .def("close_position", &PyStrategyBase::close_position, py::arg("symbol") = py::none())
      .def("cancel_order", &PyStrategyBase::cancel_order, py::arg("order_id"))
      .def("cancel_all_orders", &PyStrategyBase::cancel_all_orders,
           py::arg("symbol") = py::none())
      .def("modify_order", &PyStrategyBase::modify_order, py::arg("order_id"),
           py::arg("new_price"), py::arg("new_qty"))
      .def("pos", &PyStrategyBase::pos, py::arg("symbol") = py::none())
      .def("order_status", &PyStrategyBase::order_status, py::arg("order_id"))
      .def_property_readonly("symbol_names", &PyStrategyBase::symbol_names)
      .def_property_readonly("primary_symbol_name", &PyStrategyBase::primary_symbol_name);
}
