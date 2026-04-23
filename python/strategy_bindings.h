// python/strategy_bindings.h

#pragma once

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/book/bus/trade_bus.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/capi/bridge_strategy.h"
#include "flox/capi/flox_capi.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/util/memory/pool.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
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

  double last_price(std::optional<std::string> symbol = std::nullopt) const
  {
    if (!_bridge)
    {
      return 0.0;
    }
    return _bridge->ctx(_resolve(symbol)).lastTradePrice.toDouble();
  }

  double best_bid(std::optional<std::string> symbol = std::nullopt) const
  {
    if (!_bridge)
    {
      return 0.0;
    }
    auto bid = _bridge->ctx(_resolve(symbol)).book.bestBid();
    return bid ? bid->toDouble() : 0.0;
  }

  double best_ask(std::optional<std::string> symbol = std::nullopt) const
  {
    if (!_bridge)
    {
      return 0.0;
    }
    auto ask = _bridge->ctx(_resolve(symbol)).book.bestAsk();
    return ask ? ask->toDouble() : 0.0;
  }

  double mid_price(std::optional<std::string> symbol = std::nullopt) const
  {
    if (!_bridge)
    {
      return 0.0;
    }
    auto mid = _bridge->ctx(_resolve(symbol)).mid();
    return mid ? mid->toDouble() : 0.0;
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

// ──────────────────────────────────────────────────────────────────────
// PySignal — Python-friendly representation of an emitted order signal
// ──────────────────────────────────────────────────────────────────────

struct PySignal
{
  uint64_t order_id{0};
  uint32_t symbol{0};
  std::string side;        // "buy" or "sell"
  std::string order_type;  // "market", "limit", "stop_market", ...
  double price{0.0};
  double quantity{0.0};
  double trigger_price{0.0};
  double trailing_offset{0.0};
  int32_t trailing_bps{0};
  double new_price{0.0};
  double new_quantity{0.0};
};

inline PySignal pySignalFromC(const FloxSignal* s)
{
  static constexpr const char* kOrderTypes[] = {
      "market", "limit", "stop_market", "stop_limit",
      "tp_market", "tp_limit", "trailing_stop",
      "cancel", "cancel_all", "modify"};
  PySignal ps{};
  ps.order_id = s->order_id;
  ps.symbol = s->symbol;
  ps.side = s->side == 0 ? "buy" : "sell";
  ps.order_type = s->order_type < 10 ? kOrderTypes[s->order_type] : "unknown";
  ps.price = s->price;
  ps.quantity = s->quantity;
  ps.trigger_price = s->trigger_price;
  ps.trailing_offset = s->trailing_offset;
  ps.trailing_bps = s->trailing_bps;
  ps.new_price = s->new_price;
  ps.new_quantity = s->new_quantity;
  return ps;
}

// ──────────────────────────────────────────────────────────────────────
// PyStrategyHost — owns a BridgeStrategy, wires Python callbacks.
// with_gil: true when callbacks fire from a non-Python thread
//           (i.e., LiveEngine consumer threads).
// ──────────────────────────────────────────────────────────────────────

struct PyStrategyHost
{
  PyStrategyBase* strategy;
  std::unique_ptr<BridgeStrategy> bridge;
  bool with_gil;

  PyStrategyHost(PyStrategyBase* strat, SymbolRegistry* reg,
                 uint32_t id, bool with_gil_)
      : strategy(strat), with_gil(with_gil_)
  {
    FloxStrategyCallbacks cbs{};
    cbs.user_data = this;
    cbs.on_trade = &PyStrategyHost::onTrade;
    cbs.on_book = &PyStrategyHost::onBook;
    cbs.on_start = &PyStrategyHost::onStart;
    cbs.on_stop = &PyStrategyHost::onStop;

    const auto& syms = strat->symbols();
    bridge = std::make_unique<BridgeStrategy>(
        static_cast<SubscriberId>(id), syms, *reg, cbs);
    strat->_initBridge(bridge.get());
  }

  // Callbacks are C function pointers — no captures allowed.
  static void onTrade(void* ud, const FloxSymbolContext* ctx,
                      const FloxTradeData* trade)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    auto call = [self, ctx, trade]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = flox_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = flox_price_to_double(ctx->last_trade_price_raw);
      pc.best_bid = flox_price_to_double(ctx->book.bid_price_raw);
      pc.best_ask = flox_price_to_double(ctx->book.ask_price_raw);
      pc.mid_price = flox_price_to_double(ctx->book.mid_raw);

      PyTradeData pt{};
      pt.symbol = trade->symbol;
      pt.price = flox_price_to_double(trade->price_raw);
      pt.quantity = flox_quantity_to_double(trade->quantity_raw);
      pt.is_buy = trade->is_buy != 0;
      pt.side = pt.is_buy ? "buy" : "sell";
      pt.timestamp_ns = trade->exchange_ts_ns;

      self->strategy->on_trade(pc, pt);
    };
    if (self->with_gil)
    {
      py::gil_scoped_acquire gil;
      call();
    }
    else
    {
      call();
    }
  }

  static void onBook(void* ud, const FloxSymbolContext* ctx,
                     const FloxBookData* /*book*/)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    auto call = [self, ctx]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = flox_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = flox_price_to_double(ctx->last_trade_price_raw);
      pc.best_bid = flox_price_to_double(ctx->book.bid_price_raw);
      pc.best_ask = flox_price_to_double(ctx->book.ask_price_raw);
      pc.mid_price = flox_price_to_double(ctx->book.mid_raw);
      self->strategy->on_book_update(pc);
    };
    if (self->with_gil)
    {
      py::gil_scoped_acquire gil;
      call();
    }
    else
    {
      call();
    }
  }

  static void onStart(void* ud)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    if (self->with_gil)
    {
      py::gil_scoped_acquire gil;
      self->strategy->on_start();
    }
    else
    {
      self->strategy->on_start();
    }
  }

  static void onStop(void* ud)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    if (self->with_gil)
    {
      py::gil_scoped_acquire gil;
      self->strategy->on_stop();
    }
    else
    {
      self->strategy->on_stop();
    }
  }
};

// ──────────────────────────────────────────────────────────────────────
// PyStrategyRunner — synchronous strategy host.
//
// Market data is pushed from the Python thread (e.g. asyncio callback,
// WebSocket handler). Strategy on_trade / on_book callbacks fire
// synchronously before the push call returns. Orders emitted by the
// strategy are forwarded to the on_signal Python callable.
// ──────────────────────────────────────────────────────────────────────

class PyStrategyRunner
{
 public:
  PyStrategyRunner(SymbolRegistry* reg, py::object on_signal)
      : _reg(reg), _on_signal(std::move(on_signal))
  {
    _runner = flox_runner_create(static_cast<FloxRegistryHandle>(reg),
                                 &PyStrategyRunner::signalCallback, this);
  }

  ~PyStrategyRunner()
  {
    if (_runner)
    {
      flox_runner_destroy(_runner);
    }
  }

  void add_strategy(PyStrategyBase* strat)
  {
    uint32_t id = static_cast<uint32_t>(_hosts.size()) + 1;
    auto host = std::make_unique<PyStrategyHost>(strat, _reg, id, false);
    flox_runner_add_strategy(_runner,
                             static_cast<FloxStrategyHandle>(host->bridge.get()));
    _hosts.push_back(std::move(host));
  }

  void start() { flox_runner_start(_runner); }
  void stop() { flox_runner_stop(_runner); }

  void on_trade(uint32_t symbol, double price, double qty,
                bool is_buy, int64_t ts_ns)
  {
    flox_runner_on_trade(_runner, symbol, price, qty,
                         static_cast<uint8_t>(is_buy), ts_ns);
  }

  void on_book_snapshot(uint32_t symbol,
                        const std::vector<double>& bid_prices,
                        const std::vector<double>& bid_qtys,
                        const std::vector<double>& ask_prices,
                        const std::vector<double>& ask_qtys,
                        int64_t ts_ns)
  {
    uint32_t nb = static_cast<uint32_t>(bid_prices.size());
    uint32_t na = static_cast<uint32_t>(ask_prices.size());
    flox_runner_on_book_snapshot(_runner, symbol,
                                 bid_prices.data(), bid_qtys.data(), nb,
                                 ask_prices.data(), ask_qtys.data(), na,
                                 ts_ns);
  }

 private:
  SymbolRegistry* _reg;
  FloxRunnerHandle _runner{nullptr};
  py::object _on_signal;
  std::vector<std::unique_ptr<PyStrategyHost>> _hosts;

  static void signalCallback(void* ud, const FloxSignal* sig)
  {
    // Called synchronously from Python thread — GIL already held.
    auto* self = static_cast<PyStrategyRunner*>(ud);
    self->_on_signal(pySignalFromC(sig));
  }
};

// ──────────────────────────────────────────────────────────────────────
// PyLiveEngine — Disruptor-based live engine.
//
// Each strategy runs in its own bus consumer thread. publish_trade /
// publish_book_snapshot are lock-free and return immediately.
// Strategy callbacks and on_signal fire from C++ consumer threads;
// the GIL is acquired automatically.
// ──────────────────────────────────────────────────────────────────────

class PyLiveEngine
{
 public:
  explicit PyLiveEngine(SymbolRegistry* reg, py::object on_signal)
      : _reg(reg), _on_signal(std::move(on_signal))
  {
    _engine = flox_live_engine_create(static_cast<FloxRegistryHandle>(reg));
  }

  ~PyLiveEngine()
  {
    if (_engine)
    {
      flox_live_engine_destroy(_engine);
    }
  }

  void add_strategy(PyStrategyBase* strat)
  {
    uint32_t id = static_cast<uint32_t>(_hosts.size()) + 1;
    auto host = std::make_unique<PyStrategyHost>(strat, _reg, id, true);
    flox_live_engine_add_strategy(_engine,
                                  static_cast<FloxStrategyHandle>(host->bridge.get()),
                                  &PyLiveEngine::signalCallback, this);
    _hosts.push_back(std::move(host));
  }

  void start() { flox_live_engine_start(_engine); }
  void stop()
  {
    py::gil_scoped_release release;
    flox_live_engine_stop(_engine);
  }

  void publish_trade(uint32_t symbol, double price, double qty,
                     bool is_buy, int64_t ts_ns)
  {
    flox_live_engine_publish_trade(_engine, symbol, price, qty,
                                   static_cast<uint8_t>(is_buy), ts_ns);
  }

  void publish_book_snapshot(uint32_t symbol,
                             const std::vector<double>& bid_prices,
                             const std::vector<double>& bid_qtys,
                             const std::vector<double>& ask_prices,
                             const std::vector<double>& ask_qtys,
                             int64_t ts_ns)
  {
    uint32_t nb = static_cast<uint32_t>(bid_prices.size());
    uint32_t na = static_cast<uint32_t>(ask_prices.size());
    flox_live_engine_publish_book_snapshot(_engine, symbol,
                                           bid_prices.data(), bid_qtys.data(), nb,
                                           ask_prices.data(), ask_qtys.data(), na,
                                           ts_ns);
  }

 private:
  SymbolRegistry* _reg;
  FloxLiveEngineHandle _engine{nullptr};
  py::object _on_signal;
  std::vector<std::unique_ptr<PyStrategyHost>> _hosts;

  static void signalCallback(void* ud, const FloxSignal* sig)
  {
    // Called from C++ consumer thread — GIL must be acquired.
    auto* self = static_cast<PyLiveEngine*>(ud);
    py::gil_scoped_acquire gil;
    self->_on_signal(pySignalFromC(sig));
  }
};

// ──────────────────────────────────────────────────────────────────────
// PyRunner — unified live runner.
//
//   Runner(registry, on_signal)                — sync (StrategyRunner)
//   Runner(registry, on_signal, threaded=True) — Disruptor (LiveEngine)
//
// The same Strategy class and the same on_trade() / on_book_snapshot()
// calls work in both modes. threaded=True uses lock-free publish and
// runs strategy callbacks in a background C++ thread.
// ──────────────────────────────────────────────────────────────────────

class PyRunner
{
 public:
  PyRunner(SymbolRegistry* reg, py::object on_signal, bool threaded)
  {
    if (threaded)
    {
      _live = std::make_unique<PyLiveEngine>(reg, std::move(on_signal));
    }
    else
    {
      _sync = std::make_unique<PyStrategyRunner>(reg, std::move(on_signal));
    }
  }

  void add_strategy(PyStrategyBase* strat)
  {
    if (_live)
    {
      _live->add_strategy(strat);
    }
    else
    {
      _sync->add_strategy(strat);
    }
  }

  void start()
  {
    if (_live)
    {
      _live->start();
    }
    else
    {
      _sync->start();
    }
  }

  void stop()
  {
    if (_live)
    {
      _live->stop();
    }
    else
    {
      _sync->stop();
    }
  }

  void on_trade(uint32_t symbol, double price, double qty,
                bool is_buy, int64_t ts_ns)
  {
    if (_live)
    {
      _live->publish_trade(symbol, price, qty, is_buy, ts_ns);
    }
    else
    {
      _sync->on_trade(symbol, price, qty, is_buy, ts_ns);
    }
  }

  void on_book_snapshot(uint32_t symbol,
                        const std::vector<double>& bid_prices,
                        const std::vector<double>& bid_qtys,
                        const std::vector<double>& ask_prices,
                        const std::vector<double>& ask_qtys,
                        int64_t ts_ns)
  {
    if (_live)
    {
      _live->publish_book_snapshot(symbol, bid_prices, bid_qtys,
                                   ask_prices, ask_qtys, ts_ns);
    }
    else
    {
      _sync->on_book_snapshot(symbol, bid_prices, bid_qtys,
                              ask_prices, ask_qtys, ts_ns);
    }
  }

 private:
  std::unique_ptr<PyStrategyRunner> _sync;
  std::unique_ptr<PyLiveEngine> _live;
};

// ──────────────────────────────────────────────────────────────────────
// OhlcvBacktestReader — feeds OHLCV bars into BacktestRunner as trades.
// ──────────────────────────────────────────────────────────────────────

class OhlcvBacktestReader : public replay::IMultiSegmentReader
{
 public:
  struct Bar
  {
    int64_t ts_ns;
    int64_t price_raw;
    uint32_t symbol_id;
  };

  explicit OhlcvBacktestReader(std::vector<Bar> bars) : _bars(std::move(bars)) {}

  uint64_t forEach(EventCallback cb) override
  {
    uint64_t n = 0;
    for (const auto& bar : _bars)
    {
      if (!cb(makeEvent(bar)))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  uint64_t forEachFrom(int64_t start_ns, EventCallback cb) override
  {
    uint64_t n = 0;
    for (const auto& bar : _bars)
    {
      if (bar.ts_ns < start_ns)
      {
        continue;
      }
      if (!cb(makeEvent(bar)))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  const std::vector<replay::SegmentInfo>& segments() const override { return _segs; }
  uint64_t totalEvents() const override { return _bars.size(); }

 private:
  static replay::ReplayEvent makeEvent(const Bar& bar)
  {
    replay::ReplayEvent ev{};
    ev.type = replay::EventType::Trade;
    ev.timestamp_ns = bar.ts_ns;
    ev.trade.exchange_ts_ns = bar.ts_ns;
    ev.trade.price_raw = bar.price_raw;
    ev.trade.qty_raw = Quantity::fromDouble(1.0).raw();  // synthetic qty
    ev.trade.symbol_id = bar.symbol_id;
    ev.trade.side = 1;  // buy
    return ev;
  }

  std::vector<Bar> _bars;
  std::vector<replay::SegmentInfo> _segs;
};

// ──────────────────────────────────────────────────────────────────────
// PyBacktestRunner — same Strategy class as StrategyRunner / LiveEngine,
// but replays historical OHLCV data through it and returns stats.
// ──────────────────────────────────────────────────────────────────────

class PyBacktestRunner
{
 public:
  PyBacktestRunner(SymbolRegistry* reg, double fee_rate, double initial_capital)
      : _reg(reg)
  {
    BacktestConfig cfg{};
    cfg.feeRate = fee_rate;
    cfg.initialCapital = initial_capital;
    cfg.usePercentageFee = true;
    _runner = std::make_unique<BacktestRunner>(cfg);
  }

  void set_strategy(PyStrategyBase* strat)
  {
    uint32_t id = 1;
    // with_gil = false: backtest runs on caller's thread, GIL already held
    _host = std::make_unique<PyStrategyHost>(strat, _reg, id, false);
    // BacktestRunner sets itself as ISignalHandler on the bridge —
    // emitted orders go straight to SimulatedExecutor.
    _runner->setStrategy(_host->bridge.get());
  }

  py::object run_csv(const std::string& path, const std::string& symbol = "")
  {
    std::string sym = symbol.empty() ? inferSymbol(path) : symbol;
    auto id = resolveSymbol(sym);
    auto bars = loadCsv(path, id);
    return runBars(std::move(bars));
  }

  py::object run_ohlcv(py::array_t<int64_t, py::array::c_style | py::array::forcecast> ts,
                       py::array_t<double, py::array::c_style | py::array::forcecast> close,
                       const std::string& symbol = "")
  {
    std::string sym = symbol.empty() ? "default" : symbol;
    auto id = resolveSymbol(sym);
    auto n = ts.size();
    std::vector<OhlcvBacktestReader::Bar> bars;
    bars.reserve(n);
    auto pts = ts.unchecked<1>();
    auto pc = close.unchecked<1>();
    for (py::ssize_t i = 0; i < n; ++i)
    {
      bars.push_back({normalizeTs(pts(i)), Price::fromDouble(pc(i)).raw(), id});
    }
    return runBars(std::move(bars));
  }

 private:
  SymbolRegistry* _reg;
  std::unique_ptr<BacktestRunner> _runner;
  std::unique_ptr<PyStrategyHost> _host;

  uint32_t resolveSymbol(const std::string& sym)
  {
    auto opt = _reg->getSymbolId("", sym);
    if (!opt)
    {
      // try any exchange
      auto all = _reg->getAllSymbols();
      for (const auto& info : all)
      {
        if (info.symbol == sym)
        {
          return info.id;
        }
      }
      throw std::invalid_argument("Symbol not registered: " + sym);
    }
    return *opt;
  }

  py::object runBars(std::vector<OhlcvBacktestReader::Bar> bars)
  {
    if (!_host)
    {
      throw std::runtime_error("call set_strategy() before run");
    }
    OhlcvBacktestReader reader(std::move(bars));
    BacktestResult result = _runner->run(reader);
    BacktestStats stats = result.computeStats();
    // Return a dict — same keys as PyStats.to_dict()
    py::dict d;
    d["total_trades"] = stats.totalTrades;
    d["winning_trades"] = stats.winningTrades;
    d["losing_trades"] = stats.losingTrades;
    d["initial_capital"] = stats.initialCapital;
    d["final_capital"] = stats.finalCapital;
    d["total_pnl"] = stats.totalPnl;
    d["total_fees"] = stats.totalFees;
    d["net_pnl"] = stats.netPnl;
    d["gross_profit"] = stats.grossProfit;
    d["gross_loss"] = stats.grossLoss;
    d["max_drawdown"] = stats.maxDrawdown;
    d["max_drawdown_pct"] = stats.maxDrawdownPct;
    d["win_rate"] = stats.winRate;
    d["profit_factor"] = stats.profitFactor;
    d["sharpe"] = stats.sharpeRatio;
    d["sortino"] = stats.sortinoRatio;
    d["return_pct"] = stats.returnPct;
    return d;
  }

  static std::vector<OhlcvBacktestReader::Bar> loadCsv(const std::string& path, uint32_t id)
  {
    std::ifstream f(path);
    if (!f.is_open())
    {
      throw std::runtime_error("cannot open: " + path);
    }
    std::vector<OhlcvBacktestReader::Bar> bars;
    std::string line;
    std::getline(f, line);  // skip header
    while (std::getline(f, line))
    {
      if (line.empty())
      {
        continue;
      }
      std::istringstream ss(line);
      std::string tok;
      std::getline(ss, tok, ',');
      int64_t ts = normalizeTs(std::stoll(tok));
      std::getline(ss, tok, ',');  // open
      std::getline(ss, tok, ',');  // high
      std::getline(ss, tok, ',');  // low
      std::getline(ss, tok, ',');
      double c = std::stod(tok);
      bars.push_back({ts, Price::fromDouble(c).raw(), id});
    }
    return bars;
  }

  static int64_t normalizeTs(int64_t t)
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

  static std::string inferSymbol(const std::string& path)
  {
    auto pos = path.find_last_of('/');
    std::string name = pos != std::string::npos ? path.substr(pos + 1) : path;
    auto dot = name.find('.');
    if (dot != std::string::npos)
    {
      name = name.substr(0, dot);
    }
    for (const char* suf : {"_1m", "_5m", "_15m", "_1h", "_4h", "_1d"})
    {
      size_t sl = std::strlen(suf);
      if (name.size() > sl && name.substr(name.size() - sl) == suf)
      {
        name = name.substr(0, name.size() - sl);
        break;
      }
    }
    for (auto& c : name)
    {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return name;
  }
};

}  // namespace

// Convert a Python value (Symbol or int) to a symbol id.
inline uint32_t symId(const py::object& o)
{
  if (py::hasattr(o, "id"))
  {
    return o.attr("id").cast<uint32_t>();
  }
  return o.cast<uint32_t>();
}

// Convert a list of Python values (Symbol or int) to symbol ids.
inline std::vector<uint32_t> symIds(const py::list& lst)
{
  std::vector<uint32_t> ids;
  ids.reserve(lst.size());
  for (auto item : lst)
  {
    ids.push_back(symId(item.cast<py::object>()));
  }
  return ids;
}

struct PySymbol
{
  uint32_t id;
  std::string exchange;
  std::string name;
  double tick_size;

  std::string repr() const
  {
    return "Symbol(" + exchange + ":" + name +
           ", id=" + std::to_string(id) + ")";
  }
};

inline void bindStrategy(py::module_& m)
{
  py::class_<PySymbol>(m, "Symbol")
      .def_readonly("id", &PySymbol::id)
      .def_readonly("exchange", &PySymbol::exchange)
      .def_readonly("name", &PySymbol::name)
      .def_readonly("tick_size", &PySymbol::tick_size)
      .def("__repr__", &PySymbol::repr)
      .def("__str__", &PySymbol::repr)
      .def("__int__", [](const PySymbol& s)
           { return static_cast<int>(s.id); })
      .def("__index__", [](const PySymbol& s)
           { return static_cast<int>(s.id); })
      .def("__eq__", [](const PySymbol& a, const PySymbol& b)
           { return a.id == b.id; })
      .def("__hash__", [](const PySymbol& s)
           { return std::hash<uint32_t>{}(s.id); });

  py::class_<SymbolRegistry>(m, "SymbolRegistry")
      .def(py::init<>())
      .def("add_symbol", [](SymbolRegistry& reg, const std::string& exchange, const std::string& symbol, double tick_size) -> PySymbol
           {
             flox::SymbolInfo info;
             info.exchange  = exchange;
             info.symbol    = symbol;
             info.tickSize  = Price::fromDouble(tick_size);
             uint32_t id = static_cast<uint32_t>(reg.registerSymbol(info));
             return PySymbol{id, exchange, symbol, tick_size}; }, py::arg("exchange"), py::arg("symbol"), py::arg("tick_size") = 0.01)
      .def("symbol_count", &SymbolRegistry::size);

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
      .def(py::init([](py::list symbols)
                    { return std::make_unique<PyStrategyTrampoline>(symIds(symbols)); }),
           py::arg("symbols"))
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
      .def("last_price", &PyStrategyBase::last_price, py::arg("symbol") = py::none())
      .def("best_bid", &PyStrategyBase::best_bid, py::arg("symbol") = py::none())
      .def("best_ask", &PyStrategyBase::best_ask, py::arg("symbol") = py::none())
      .def("mid_price", &PyStrategyBase::mid_price, py::arg("symbol") = py::none())
      .def("order_status", &PyStrategyBase::order_status, py::arg("order_id"))
      .def_property_readonly("symbol_names", &PyStrategyBase::symbol_names)
      .def_property_readonly("primary_symbol_name", &PyStrategyBase::primary_symbol_name);

  py::class_<PySignal>(m, "Signal")
      .def(py::init<>())
      .def_readwrite("order_id", &PySignal::order_id)
      .def_readwrite("symbol", &PySignal::symbol)
      .def_readwrite("side", &PySignal::side)
      .def_readwrite("order_type", &PySignal::order_type)
      .def_readwrite("price", &PySignal::price)
      .def_readwrite("quantity", &PySignal::quantity)
      .def_readwrite("trigger_price", &PySignal::trigger_price)
      .def_readwrite("trailing_offset", &PySignal::trailing_offset)
      .def_readwrite("trailing_bps", &PySignal::trailing_bps)
      .def_readwrite("new_price", &PySignal::new_price)
      .def_readwrite("new_quantity", &PySignal::new_quantity);

  py::class_<PyRunner>(m, "Runner")
      .def(py::init([](SymbolRegistry* reg, py::object on_signal, bool threaded)
                    { return std::make_unique<PyRunner>(reg, std::move(on_signal), threaded); }),
           py::arg("registry"), py::arg("on_signal"), py::arg("threaded") = false,
           py::keep_alive<1, 2>())
      .def("add_strategy", &PyRunner::add_strategy, py::arg("strategy"),
           py::keep_alive<1, 2>())
      .def("start", &PyRunner::start)
      .def("stop", &PyRunner::stop)
      .def("on_trade", [](PyRunner& r, py::object sym, double price, double qty, bool is_buy, int64_t ts_ns)
           { r.on_trade(symId(sym), price, qty, is_buy, ts_ns); }, py::arg("symbol"), py::arg("price"), py::arg("qty"), py::arg("is_buy"), py::arg("ts_ns") = 0)
      .def("on_book_snapshot", [](PyRunner& r, py::object sym, const std::vector<double>& bp, const std::vector<double>& bq, const std::vector<double>& ap, const std::vector<double>& aq, int64_t ts_ns)
           { r.on_book_snapshot(symId(sym), bp, bq, ap, aq, ts_ns); }, py::arg("symbol"), py::arg("bid_prices"), py::arg("bid_qtys"), py::arg("ask_prices"), py::arg("ask_qtys"), py::arg("ts_ns") = 0);

  py::class_<PyBacktestRunner>(m, "BacktestRunner")
      .def(py::init([](SymbolRegistry* reg, double fee_rate, double initial_capital)
                    { return std::make_unique<PyBacktestRunner>(reg, fee_rate, initial_capital); }),
           py::arg("registry"),
           py::arg("fee_rate") = 0.0004,
           py::arg("initial_capital") = 100000.0,
           py::keep_alive<1, 2>())
      .def("set_strategy", &PyBacktestRunner::set_strategy, py::arg("strategy"),
           py::keep_alive<1, 2>())
      .def("run_csv", &PyBacktestRunner::run_csv,
           py::arg("path"), py::arg("symbol") = "")
      .def("run_ohlcv", &PyBacktestRunner::run_ohlcv,
           py::arg("ts"), py::arg("close"), py::arg("symbol") = "");
}
