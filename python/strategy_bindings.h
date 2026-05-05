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
#include "flox/error/flox_error.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/util/memory/pool.h"
#include "hook_bindings.h"
#include "types_bindings.h"

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

// PyTradeData lives in types_bindings.h (shared with hook_bindings.h).

struct PyBarData
{
  uint32_t symbol;
  std::string symbol_name;
  uint8_t bar_type;
  uint64_t bar_type_param;
  double open;
  double high;
  double low;
  double close;
  double volume;
  double buy_volume;
  int64_t start_time_ns;
  int64_t end_time_ns;
  uint8_t close_reason;
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
  virtual void on_bar(const PySymbolCtx& ctx, const PyBarData& bar) {}
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
    throw flox::FloxError(
        "E_SYM_001",
        "Symbol '" + symbol.value() +
            "' is not registered. Add it via Engine.add_symbol() "
            "before referencing it.");
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

  void on_bar(const PySymbolCtx& ctx, const PyBarData& bar) override
  {
    PYBIND11_OVERRIDE(void, PyStrategyBase, on_bar, ctx, bar);
  }

  void on_start() override { PYBIND11_OVERRIDE(void, PyStrategyBase, on_start); }

  void on_stop() override { PYBIND11_OVERRIDE(void, PyStrategyBase, on_stop); }
};

// PySignal + pySignalFromC live in types_bindings.h (shared with hook_bindings.h).

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
    cbs.on_bar = &PyStrategyHost::onBar;
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

  static void onBar(void* ud, const FloxSymbolContext* ctx,
                    const FloxBarData* bar)
  {
    auto* self = static_cast<PyStrategyHost*>(ud);
    auto call = [self, ctx, bar]()
    {
      PySymbolCtx pc{};
      pc.symbol_id = ctx->symbol_id;
      pc.position = flox_quantity_to_double(ctx->position_raw);
      pc.last_trade_price = flox_price_to_double(ctx->last_trade_price_raw);
      pc.best_bid = flox_price_to_double(ctx->book.bid_price_raw);
      pc.best_ask = flox_price_to_double(ctx->book.ask_price_raw);
      pc.mid_price = flox_price_to_double(ctx->book.mid_raw);

      PyBarData pb{};
      pb.symbol = bar->symbol;
      pb.bar_type = bar->bar_type;
      pb.bar_type_param = bar->bar_type_param;
      pb.open = flox_price_to_double(bar->open_raw);
      pb.high = flox_price_to_double(bar->high_raw);
      pb.low = flox_price_to_double(bar->low_raw);
      pb.close = flox_price_to_double(bar->close_raw);
      pb.volume = flox_quantity_to_double(bar->volume_raw);
      pb.buy_volume = flox_quantity_to_double(bar->buy_volume_raw);
      pb.start_time_ns = bar->start_time_ns;
      pb.end_time_ns = bar->end_time_ns;
      pb.close_reason = bar->close_reason;

      self->strategy->on_bar(pc, pb);
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

  void set_pnl_tracker(std::shared_ptr<flox_py::PyPnLTracker> tracker)
  {
    _pnl_owner.reset();
    if (tracker)
    {
      _pnl_owner = std::make_unique<flox_py::PyPnLTrackerOwner>(std::move(tracker));
      flox_runner_set_pnl_tracker(_runner, _pnl_owner->handle());
    }
    else
    {
      flox_runner_set_pnl_tracker(_runner, nullptr);
    }
  }

  void set_storage_sink(std::shared_ptr<flox_py::PyStorageSink> sink)
  {
    _storage_owner.reset();
    if (sink)
    {
      _storage_owner = std::make_unique<flox_py::PyStorageSinkOwner>(std::move(sink));
      flox_runner_set_storage_sink(_runner, _storage_owner->handle());
    }
    else
    {
      flox_runner_set_storage_sink(_runner, nullptr);
    }
  }

  void set_risk_manager(std::shared_ptr<flox_py::PyRiskManager> rm)
  {
    _risk_owner.reset();
    if (rm)
    {
      _risk_owner = std::make_unique<flox_py::PyRiskManagerOwner>(std::move(rm));
      flox_runner_set_risk_manager(_runner, _risk_owner->handle());
    }
    else
    {
      flox_runner_set_risk_manager(_runner, nullptr);
    }
  }

  void set_kill_switch(std::shared_ptr<flox_py::PyKillSwitch> ks)
  {
    _kill_owner.reset();
    if (ks)
    {
      _kill_owner = std::make_unique<flox_py::PyKillSwitchOwner>(std::move(ks));
      flox_runner_set_kill_switch(_runner, _kill_owner->handle());
    }
    else
    {
      flox_runner_set_kill_switch(_runner, nullptr);
    }
  }

  void set_order_validator(std::shared_ptr<flox_py::PyOrderValidator> ov)
  {
    _validator_owner.reset();
    if (ov)
    {
      _validator_owner = std::make_unique<flox_py::PyOrderValidatorOwner>(std::move(ov));
      flox_runner_set_order_validator(_runner, _validator_owner->handle());
    }
    else
    {
      flox_runner_set_order_validator(_runner, nullptr);
    }
  }

  void set_market_data_recorder(std::shared_ptr<flox_py::PyMarketDataRecorderHook> rec)
  {
    _recorder_owner.reset();
    if (rec)
    {
      _recorder_owner = std::make_unique<flox_py::PyMarketDataRecorderHookOwner>(std::move(rec));
      flox_runner_set_market_data_recorder(_runner, _recorder_owner->handle());
    }
    else
    {
      flox_runner_set_market_data_recorder(_runner, nullptr);
    }
  }

  void set_executor(std::shared_ptr<flox_py::PyExecutor> exec)
  {
    _executor_owner.reset();
    if (exec)
    {
      _executor_owner = std::make_unique<flox_py::PyExecutorOwner>(std::move(exec));
      flox_runner_set_executor(_runner, _executor_owner->handle());
    }
    else
    {
      flox_runner_set_executor(_runner, nullptr);
    }
  }

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

  void on_bar(uint32_t symbol,
              double open, double high, double low, double close,
              double volume, double buy_volume,
              int64_t start_time_ns, int64_t end_time_ns,
              uint8_t bar_type = 0, uint64_t bar_type_param = 0,
              uint8_t close_reason = 0)
  {
    flox_runner_on_bar(_runner, symbol, bar_type, bar_type_param,
                       open, high, low, close, volume, buy_volume,
                       start_time_ns, end_time_ns, close_reason);
  }

 private:
  SymbolRegistry* _reg;
  FloxRunnerHandle _runner{nullptr};
  py::object _on_signal;
  std::vector<std::unique_ptr<PyStrategyHost>> _hosts;

  // Hook owners — keep the Owner alive while the runner holds a non-owning
  // FloxXxxHandle. Reset on detach (set_xxx(None)) or on destruction.
  std::unique_ptr<flox_py::PyPnLTrackerOwner> _pnl_owner;
  std::unique_ptr<flox_py::PyStorageSinkOwner> _storage_owner;
  std::unique_ptr<flox_py::PyRiskManagerOwner> _risk_owner;
  std::unique_ptr<flox_py::PyKillSwitchOwner> _kill_owner;
  std::unique_ptr<flox_py::PyOrderValidatorOwner> _validator_owner;
  std::unique_ptr<flox_py::PyMarketDataRecorderHookOwner> _recorder_owner;
  std::unique_ptr<flox_py::PyExecutorOwner> _executor_owner;

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

  void publish_bar(uint32_t symbol, uint8_t bar_type, uint64_t bar_type_param,
                   double open, double high, double low, double close,
                   double volume, double buy_volume,
                   int64_t start_time_ns, int64_t end_time_ns,
                   uint8_t close_reason)
  {
    flox_live_engine_publish_bar(_engine, symbol, bar_type, bar_type_param,
                                 open, high, low, close, volume, buy_volume,
                                 start_time_ns, end_time_ns, close_reason);
  }

  void set_pnl_tracker(std::shared_ptr<flox_py::PyPnLTracker> tracker)
  {
    _pnl_owner.reset();
    if (tracker)
    {
      _pnl_owner = std::make_unique<flox_py::PyPnLTrackerOwner>(std::move(tracker));
      flox_live_engine_set_pnl_tracker(_engine, _pnl_owner->handle());
    }
    else
    {
      flox_live_engine_set_pnl_tracker(_engine, nullptr);
    }
  }

  void set_storage_sink(std::shared_ptr<flox_py::PyStorageSink> sink)
  {
    _storage_owner.reset();
    if (sink)
    {
      _storage_owner = std::make_unique<flox_py::PyStorageSinkOwner>(std::move(sink));
      flox_live_engine_set_storage_sink(_engine, _storage_owner->handle());
    }
    else
    {
      flox_live_engine_set_storage_sink(_engine, nullptr);
    }
  }

  void set_risk_manager(std::shared_ptr<flox_py::PyRiskManager> rm)
  {
    _risk_owner.reset();
    if (rm)
    {
      _risk_owner = std::make_unique<flox_py::PyRiskManagerOwner>(std::move(rm));
      flox_live_engine_set_risk_manager(_engine, _risk_owner->handle());
    }
    else
    {
      flox_live_engine_set_risk_manager(_engine, nullptr);
    }
  }

  void set_kill_switch(std::shared_ptr<flox_py::PyKillSwitch> ks)
  {
    _kill_owner.reset();
    if (ks)
    {
      _kill_owner = std::make_unique<flox_py::PyKillSwitchOwner>(std::move(ks));
      flox_live_engine_set_kill_switch(_engine, _kill_owner->handle());
    }
    else
    {
      flox_live_engine_set_kill_switch(_engine, nullptr);
    }
  }

  void set_order_validator(std::shared_ptr<flox_py::PyOrderValidator> ov)
  {
    _validator_owner.reset();
    if (ov)
    {
      _validator_owner = std::make_unique<flox_py::PyOrderValidatorOwner>(std::move(ov));
      flox_live_engine_set_order_validator(_engine, _validator_owner->handle());
    }
    else
    {
      flox_live_engine_set_order_validator(_engine, nullptr);
    }
  }

  void set_market_data_recorder(std::shared_ptr<flox_py::PyMarketDataRecorderHook> rec)
  {
    _recorder_owner.reset();
    if (rec)
    {
      _recorder_owner = std::make_unique<flox_py::PyMarketDataRecorderHookOwner>(std::move(rec));
      flox_live_engine_set_market_data_recorder(_engine, _recorder_owner->handle());
    }
    else
    {
      flox_live_engine_set_market_data_recorder(_engine, nullptr);
    }
  }

  void set_executor(std::shared_ptr<flox_py::PyExecutor> exec)
  {
    _executor_owner.reset();
    if (exec)
    {
      _executor_owner = std::make_unique<flox_py::PyExecutorOwner>(std::move(exec));
      flox_live_engine_set_executor(_engine, _executor_owner->handle());
    }
    else
    {
      flox_live_engine_set_executor(_engine, nullptr);
    }
  }

 private:
  SymbolRegistry* _reg;
  FloxLiveEngineHandle _engine{nullptr};
  py::object _on_signal;
  std::vector<std::unique_ptr<PyStrategyHost>> _hosts;
  std::unique_ptr<flox_py::PyPnLTrackerOwner> _pnl_owner;
  std::unique_ptr<flox_py::PyStorageSinkOwner> _storage_owner;
  std::unique_ptr<flox_py::PyRiskManagerOwner> _risk_owner;
  std::unique_ptr<flox_py::PyKillSwitchOwner> _kill_owner;
  std::unique_ptr<flox_py::PyOrderValidatorOwner> _validator_owner;
  std::unique_ptr<flox_py::PyMarketDataRecorderHookOwner> _recorder_owner;
  std::unique_ptr<flox_py::PyExecutorOwner> _executor_owner;

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

#define _PYRUNNER_SET_HOOK(NAME, TYPE)                 \
  void set_##NAME(std::shared_ptr<flox_py::TYPE> hook) \
  {                                                    \
    if (_live)                                         \
    {                                                  \
      _live->set_##NAME(std::move(hook));              \
    }                                                  \
    else                                               \
    {                                                  \
      _sync->set_##NAME(std::move(hook));              \
    }                                                  \
  }
  _PYRUNNER_SET_HOOK(pnl_tracker, PyPnLTracker)
  _PYRUNNER_SET_HOOK(storage_sink, PyStorageSink)
  _PYRUNNER_SET_HOOK(risk_manager, PyRiskManager)
  _PYRUNNER_SET_HOOK(kill_switch, PyKillSwitch)
  _PYRUNNER_SET_HOOK(order_validator, PyOrderValidator)
  _PYRUNNER_SET_HOOK(market_data_recorder, PyMarketDataRecorderHook)
  _PYRUNNER_SET_HOOK(executor, PyExecutor)
#undef _PYRUNNER_SET_HOOK

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

  void on_bar(uint32_t symbol,
              double open, double high, double low, double close,
              double volume, double buy_volume,
              int64_t start_time_ns, int64_t end_time_ns,
              uint8_t bar_type = 0, uint64_t bar_type_param = 0,
              uint8_t close_reason = 0)
  {
    if (_live)
    {
      _live->publish_bar(symbol, bar_type, bar_type_param,
                         open, high, low, close, volume, buy_volume,
                         start_time_ns, end_time_ns, close_reason);
    }
    else
    {
      _sync->on_bar(symbol, open, high, low, close, volume, buy_volume,
                    start_time_ns, end_time_ns, bar_type, bar_type_param,
                    close_reason);
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

  void set_executor(std::shared_ptr<flox_py::PyExecutor> exec)
  {
    if (exec)
    {
      _executor_adapter =
          std::make_unique<flox_py::cxx_adapters::PyExecutorCxxAdapter>(std::move(exec));
      _runner->setExecutor(_executor_adapter.get());
    }
    else
    {
      _runner->setExecutor(nullptr);
      _executor_adapter.reset();
    }
  }

  void add_execution_listener(std::shared_ptr<flox_py::PyExecutionListener> listener)
  {
    if (!listener)
    {
      return;
    }
    auto id = static_cast<flox::SubscriberId>(_listener_adapters.size() + 1);
    auto adapter = std::make_unique<flox_py::cxx_adapters::PyExecutionListenerCxxAdapter>(
        id, std::move(listener));
    _runner->addExecutionListener(adapter.get());
    _listener_adapters.push_back(std::move(adapter));
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

  // Replay full OHLCV bars. Strategy.on_bar fires; on_trade does NOT.
  // bar_type: 0=Time (default), 1=Tick, 2=Volume, 3=Renko, 4=Range, 5=HeikinAshi.
  py::object run_bars(
      py::array_t<int64_t, py::array::c_style | py::array::forcecast> start_ns,
      py::array_t<int64_t, py::array::c_style | py::array::forcecast> end_ns,
      py::array_t<double, py::array::c_style | py::array::forcecast> open,
      py::array_t<double, py::array::c_style | py::array::forcecast> high,
      py::array_t<double, py::array::c_style | py::array::forcecast> low,
      py::array_t<double, py::array::c_style | py::array::forcecast> close,
      py::array_t<double, py::array::c_style | py::array::forcecast> volume,
      const std::string& symbol = "",
      uint8_t bar_type = 0,
      uint64_t bar_type_param = 0)
  {
    if (!_host)
    {
      throw flox::FloxError(
          "E_RUN_001",
          "BacktestRunner.run_bars() called before set_strategy(). "
          "Attach a strategy with set_strategy(MyStrategy()) first.");
    }
    std::string sym = symbol.empty() ? "default" : symbol;
    auto id = resolveSymbol(sym);
    const auto n = start_ns.size();
    if (end_ns.size() != n || open.size() != n || high.size() != n || low.size() != n || close.size() != n || volume.size() != n)
    {
      throw flox::FloxError(
          "E_LEN_001",
          "run_bars: all arrays (start_time_ns, end_time_ns, open, high, "
          "low, close, volume) must have the same length.");
    }
    auto ps = start_ns.unchecked<1>();
    auto pe = end_ns.unchecked<1>();
    auto po = open.unchecked<1>();
    auto ph = high.unchecked<1>();
    auto pl = low.unchecked<1>();
    auto pc = close.unchecked<1>();
    auto pv = volume.unchecked<1>();
    std::vector<BarEvent> events;
    events.reserve(n);
    for (py::ssize_t i = 0; i < n; ++i)
    {
      BarEvent ev{};
      ev.symbol = id;
      ev.barType = static_cast<BarType>(bar_type);
      ev.barTypeParam = bar_type_param;
      ev.bar.open = Price::fromDouble(po(i));
      ev.bar.high = Price::fromDouble(ph(i));
      ev.bar.low = Price::fromDouble(pl(i));
      ev.bar.close = Price::fromDouble(pc(i));
      ev.bar.volume = Volume::fromDouble(pv(i));
      ev.bar.startTime = TimePoint{std::chrono::nanoseconds{normalizeTs(ps(i))}};
      ev.bar.endTime = TimePoint{std::chrono::nanoseconds{normalizeTs(pe(i))}};
      ev.bar.reason = BarCloseReason::Threshold;
      events.push_back(ev);
    }
    BacktestResult result = _runner->runBars(events);
    BacktestStats stats = result.computeStats();
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

 private:
  SymbolRegistry* _reg;
  std::unique_ptr<BacktestRunner> _runner;
  std::unique_ptr<PyStrategyHost> _host;
  // C++ adapters bridging Python hooks → flox::IOrderExecutor /
  // IOrderExecutionListener used by BacktestRunner directly (without
  // going through the C ABI).
  std::unique_ptr<flox_py::cxx_adapters::PyExecutorCxxAdapter> _executor_adapter;
  std::vector<std::unique_ptr<flox_py::cxx_adapters::PyExecutionListenerCxxAdapter>>
      _listener_adapters;

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
      throw flox::FloxError(
          "E_SYM_001",
          "Symbol '" + sym +
              "' is not registered. Register it via "
              "BacktestRunner.set_strategy(...) or by calling "
              "Engine.add_symbol() before running.");
    }
    return *opt;
  }

  py::object runBars(std::vector<OhlcvBacktestReader::Bar> bars)
  {
    if (!_host)
    {
      throw flox::FloxError(
          "E_RUN_001",
          "BacktestRunner.run_*() called before set_strategy(). "
          "Attach a strategy with set_strategy(MyStrategy()) first.");
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
      throw flox::FloxError(
          "E_IO_001",
          "Cannot open file: '" + path +
              "'. Check the path is correct and the file is readable.");
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

  py::class_<PyBarData>(m, "BarData")
      .def(py::init<>())
      .def_readwrite("symbol", &PyBarData::symbol)
      .def_readwrite("symbol_name", &PyBarData::symbol_name)
      .def_readwrite("bar_type", &PyBarData::bar_type)
      .def_readwrite("bar_type_param", &PyBarData::bar_type_param)
      .def_readwrite("open", &PyBarData::open)
      .def_readwrite("high", &PyBarData::high)
      .def_readwrite("low", &PyBarData::low)
      .def_readwrite("close", &PyBarData::close)
      .def_readwrite("volume", &PyBarData::volume)
      .def_readwrite("buy_volume", &PyBarData::buy_volume)
      .def_readwrite("start_time_ns", &PyBarData::start_time_ns)
      .def_readwrite("end_time_ns", &PyBarData::end_time_ns)
      .def_readwrite("close_reason", &PyBarData::close_reason);

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
      .def("on_bar", &PyStrategyBase::on_bar, py::arg("ctx"), py::arg("bar"))
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

  // ── Extension-hook base classes (users subclass these) ──────────────

  py::class_<flox_py::PyPnLTracker, flox_py::PyPnLTrackerTrampoline,
             std::shared_ptr<flox_py::PyPnLTracker>>(m, "PnLTracker")
      .def(py::init<>())
      .def("on_signal", &flox_py::PyPnLTracker::on_signal, py::arg("signal"));

  py::class_<flox_py::PyStorageSink, flox_py::PyStorageSinkTrampoline,
             std::shared_ptr<flox_py::PyStorageSink>>(m, "StorageSink")
      .def(py::init<>())
      .def("store", &flox_py::PyStorageSink::store, py::arg("signal"));

  py::class_<flox_py::PyRiskManager, flox_py::PyRiskManagerTrampoline,
             std::shared_ptr<flox_py::PyRiskManager>>(m, "RiskManager")
      .def(py::init<>())
      .def("allow", &flox_py::PyRiskManager::allow, py::arg("signal"));

  py::class_<flox_py::PyKillSwitch, flox_py::PyKillSwitchTrampoline,
             std::shared_ptr<flox_py::PyKillSwitch>>(m, "KillSwitch")
      .def(py::init<>())
      .def("check", &flox_py::PyKillSwitch::check, py::arg("signal"));

  py::class_<flox_py::PyOrderValidator, flox_py::PyOrderValidatorTrampoline,
             std::shared_ptr<flox_py::PyOrderValidator>>(m, "OrderValidator")
      .def(py::init<>())
      .def("validate", &flox_py::PyOrderValidator::validate, py::arg("signal"));

  py::class_<flox_py::PyMarketDataRecorderHook,
             flox_py::PyMarketDataRecorderHookTrampoline,
             std::shared_ptr<flox_py::PyMarketDataRecorderHook>>(
      m, "MarketDataRecorderHook")
      .def(py::init<>())
      .def("on_trade", &flox_py::PyMarketDataRecorderHook::on_trade)
      .def("on_book_update", &flox_py::PyMarketDataRecorderHook::on_book_update,
           py::arg("symbol"), py::arg("is_snapshot"), py::arg("bids"),
           py::arg("asks"), py::arg("ts_ns"))
      .def("on_start", &flox_py::PyMarketDataRecorderHook::on_start)
      .def("on_stop", &flox_py::PyMarketDataRecorderHook::on_stop);

  py::class_<flox_py::PyReplayEvent>(m, "ReplayEvent")
      .def(py::init<>())
      .def_readwrite("type", &flox_py::PyReplayEvent::type)
      .def_readwrite("timestamp_ns", &flox_py::PyReplayEvent::timestamp_ns)
      .def_readwrite("trade_symbol", &flox_py::PyReplayEvent::trade_symbol)
      .def_readwrite("trade_is_buy", &flox_py::PyReplayEvent::trade_is_buy)
      .def_readwrite("trade_price", &flox_py::PyReplayEvent::trade_price)
      .def_readwrite("trade_quantity", &flox_py::PyReplayEvent::trade_quantity)
      .def_readwrite("book_symbol", &flox_py::PyReplayEvent::book_symbol)
      .def_readwrite("bids", &flox_py::PyReplayEvent::bids)
      .def_readwrite("asks", &flox_py::PyReplayEvent::asks);

  py::class_<flox_py::PyReplaySource, flox_py::PyReplaySourceTrampoline,
             std::shared_ptr<flox_py::PyReplaySource>>(m, "ReplaySource")
      .def(py::init<>())
      .def("on_start", &flox_py::PyReplaySource::on_start)
      .def("on_stop", &flox_py::PyReplaySource::on_stop)
      .def("seek_to", &flox_py::PyReplaySource::seek_to, py::arg("ts_ns"))
      .def("next", &flox_py::PyReplaySource::next);

  py::class_<flox_py::PyOrder>(m, "Order")
      .def(py::init<>())
      .def_readwrite("id", &flox_py::PyOrder::id)
      .def_readwrite("client_order_id", &flox_py::PyOrder::client_order_id)
      .def_readwrite("symbol", &flox_py::PyOrder::symbol)
      .def_readwrite("strategy_id", &flox_py::PyOrder::strategy_id)
      .def_readwrite("order_tag", &flox_py::PyOrder::order_tag)
      .def_readwrite("side", &flox_py::PyOrder::side)
      .def_readwrite("order_type", &flox_py::PyOrder::order_type)
      .def_readwrite("time_in_force", &flox_py::PyOrder::time_in_force)
      .def_readwrite("reduce_only", &flox_py::PyOrder::reduce_only)
      .def_readwrite("post_only", &flox_py::PyOrder::post_only)
      .def_readwrite("close_position", &flox_py::PyOrder::close_position)
      .def_readwrite("price", &flox_py::PyOrder::price)
      .def_readwrite("quantity", &flox_py::PyOrder::quantity)
      .def_readwrite("filled_quantity", &flox_py::PyOrder::filled_quantity)
      .def_readwrite("trigger_price", &flox_py::PyOrder::trigger_price)
      .def_readwrite("trailing_offset", &flox_py::PyOrder::trailing_offset)
      .def_readwrite("created_at_ns", &flox_py::PyOrder::created_at_ns)
      .def_readwrite("exchange_ts_ns", &flox_py::PyOrder::exchange_ts_ns);

  py::class_<flox_py::PyExchangeCapabilities>(m, "ExchangeCapabilities")
      .def(py::init<>())
      .def_readwrite("stop_market", &flox_py::PyExchangeCapabilities::stop_market)
      .def_readwrite("stop_limit", &flox_py::PyExchangeCapabilities::stop_limit)
      .def_readwrite("take_profit_market",
                     &flox_py::PyExchangeCapabilities::take_profit_market)
      .def_readwrite("take_profit_limit",
                     &flox_py::PyExchangeCapabilities::take_profit_limit)
      .def_readwrite("trailing_stop", &flox_py::PyExchangeCapabilities::trailing_stop)
      .def_readwrite("iceberg", &flox_py::PyExchangeCapabilities::iceberg)
      .def_readwrite("oco", &flox_py::PyExchangeCapabilities::oco)
      .def_readwrite("gtc", &flox_py::PyExchangeCapabilities::gtc)
      .def_readwrite("ioc", &flox_py::PyExchangeCapabilities::ioc)
      .def_readwrite("fok", &flox_py::PyExchangeCapabilities::fok)
      .def_readwrite("gtd", &flox_py::PyExchangeCapabilities::gtd)
      .def_readwrite("post_only", &flox_py::PyExchangeCapabilities::post_only)
      .def_readwrite("reduce_only", &flox_py::PyExchangeCapabilities::reduce_only)
      .def_readwrite("close_position", &flox_py::PyExchangeCapabilities::close_position);

  py::class_<flox_py::PyExecutor, flox_py::PyExecutorTrampoline,
             std::shared_ptr<flox_py::PyExecutor>>(m, "Executor")
      .def(py::init<>())
      .def("submit", &flox_py::PyExecutor::submit, py::arg("order"))
      .def("cancel", &flox_py::PyExecutor::cancel, py::arg("order_id"))
      .def("cancel_all", &flox_py::PyExecutor::cancel_all, py::arg("symbol"))
      .def("replace", &flox_py::PyExecutor::replace, py::arg("old_order_id"),
           py::arg("new_order"))
      .def("submit_oco", &flox_py::PyExecutor::submit_oco, py::arg("order1"),
           py::arg("order2"))
      .def("capabilities", &flox_py::PyExecutor::capabilities)
      .def("on_start", &flox_py::PyExecutor::on_start)
      .def("on_stop", &flox_py::PyExecutor::on_stop);

  py::class_<flox_py::PyExecutionListener, flox_py::PyExecutionListenerTrampoline,
             std::shared_ptr<flox_py::PyExecutionListener>>(m, "ExecutionListener")
      .def(py::init<>())
      .def("on_submitted", &flox_py::PyExecutionListener::on_submitted)
      .def("on_accepted", &flox_py::PyExecutionListener::on_accepted)
      .def("on_partially_filled", &flox_py::PyExecutionListener::on_partially_filled,
           py::arg("order"), py::arg("fill_qty"))
      .def("on_filled", &flox_py::PyExecutionListener::on_filled)
      .def("on_pending_cancel", &flox_py::PyExecutionListener::on_pending_cancel)
      .def("on_canceled", &flox_py::PyExecutionListener::on_canceled)
      .def("on_expired", &flox_py::PyExecutionListener::on_expired)
      .def("on_rejected", &flox_py::PyExecutionListener::on_rejected,
           py::arg("order"), py::arg("reason"))
      .def("on_replaced", &flox_py::PyExecutionListener::on_replaced,
           py::arg("old_order"), py::arg("new_order"))
      .def("on_pending_trigger", &flox_py::PyExecutionListener::on_pending_trigger)
      .def("on_triggered", &flox_py::PyExecutionListener::on_triggered)
      .def("on_trailing_stop_updated",
           &flox_py::PyExecutionListener::on_trailing_stop_updated,
           py::arg("order"), py::arg("new_trigger"));

  m.def("set_log_callback", &flox_py::setPythonLogCallback, py::arg("callback"),
        "Install a Python callable as the global log sink. Pass None to "
        "detach. Callable receives (level: int, msg: str); level: 0=info, "
        "1=warn, 2=error.");

  py::class_<PyRunner>(m, "Runner")
      .def(py::init([](SymbolRegistry* reg, py::object on_signal, bool threaded)
                    { return std::make_unique<PyRunner>(reg, std::move(on_signal), threaded); }),
           py::arg("registry"), py::arg("on_signal"), py::arg("threaded") = false,
           py::keep_alive<1, 2>())
      .def("add_strategy", &PyRunner::add_strategy, py::arg("strategy"),
           py::keep_alive<1, 2>())
      .def("start", &PyRunner::start)
      .def("stop", &PyRunner::stop)
      .def("set_pnl_tracker", &PyRunner::set_pnl_tracker, py::arg("tracker"),
           py::keep_alive<1, 2>())
      .def("set_storage_sink", &PyRunner::set_storage_sink, py::arg("sink"),
           py::keep_alive<1, 2>())
      .def("set_risk_manager", &PyRunner::set_risk_manager, py::arg("rm"),
           py::keep_alive<1, 2>())
      .def("set_kill_switch", &PyRunner::set_kill_switch, py::arg("ks"),
           py::keep_alive<1, 2>())
      .def("set_order_validator", &PyRunner::set_order_validator, py::arg("ov"),
           py::keep_alive<1, 2>())
      .def("set_market_data_recorder", &PyRunner::set_market_data_recorder,
           py::arg("recorder"), py::keep_alive<1, 2>())
      .def("set_executor", &PyRunner::set_executor, py::arg("executor"),
           py::keep_alive<1, 2>())
      .def("on_trade", [](PyRunner& r, py::object sym, double price, double qty, bool is_buy, int64_t ts_ns)
           { r.on_trade(symId(sym), price, qty, is_buy, ts_ns); }, py::arg("symbol"), py::arg("price"), py::arg("qty"), py::arg("is_buy"), py::arg("ts_ns") = 0)
      .def("on_book_snapshot", [](PyRunner& r, py::object sym, const std::vector<double>& bp, const std::vector<double>& bq, const std::vector<double>& ap, const std::vector<double>& aq, int64_t ts_ns)
           { r.on_book_snapshot(symId(sym), bp, bq, ap, aq, ts_ns); }, py::arg("symbol"), py::arg("bid_prices"), py::arg("bid_qtys"), py::arg("ask_prices"), py::arg("ask_qtys"), py::arg("ts_ns") = 0)
      .def("on_bar", [](PyRunner& r, py::object sym, double o, double h, double l, double c, double v, double bv, int64_t start_ns, int64_t end_ns, uint8_t bar_type, uint64_t bar_type_param, uint8_t close_reason)
           { r.on_bar(symId(sym), o, h, l, c, v, bv, start_ns, end_ns,
                      bar_type, bar_type_param, close_reason); }, py::arg("symbol"), py::arg("open"), py::arg("high"), py::arg("low"), py::arg("close"), py::arg("volume") = 0.0, py::arg("buy_volume") = 0.0, py::arg("start_time_ns") = 0, py::arg("end_time_ns") = 0, py::arg("bar_type") = 0, py::arg("bar_type_param") = 0, py::arg("close_reason") = 0);

  py::class_<PyBacktestRunner>(m, "BacktestRunner")
      .def(py::init([](SymbolRegistry* reg, double fee_rate, double initial_capital)
                    { return std::make_unique<PyBacktestRunner>(reg, fee_rate, initial_capital); }),
           py::arg("registry"),
           py::arg("fee_rate") = 0.0004,
           py::arg("initial_capital") = 100000.0,
           py::keep_alive<1, 2>())
      .def("set_strategy", &PyBacktestRunner::set_strategy, py::arg("strategy"),
           py::keep_alive<1, 2>())
      .def("set_executor", &PyBacktestRunner::set_executor, py::arg("executor"),
           py::keep_alive<1, 2>())
      .def("add_execution_listener", &PyBacktestRunner::add_execution_listener,
           py::arg("listener"), py::keep_alive<1, 2>())
      .def("run_csv", &PyBacktestRunner::run_csv,
           py::arg("path"), py::arg("symbol") = "")
      .def("run_ohlcv", &PyBacktestRunner::run_ohlcv,
           py::arg("ts"), py::arg("close"), py::arg("symbol") = "")
      .def("run_bars", &PyBacktestRunner::run_bars,
           py::arg("start_time_ns"), py::arg("end_time_ns"),
           py::arg("open"), py::arg("high"), py::arg("low"), py::arg("close"),
           py::arg("volume"),
           py::arg("symbol") = "",
           py::arg("bar_type") = 0,
           py::arg("bar_type_param") = 0);
}
