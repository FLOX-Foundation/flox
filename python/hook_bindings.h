/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// pybind11 wrappers for the C-ABI extension hooks.
//
// Pattern (used for every hook):
//
//   PyXxx           — Pythonic base class users subclass; virtual methods.
//   PyXxxTrampoline — pybind11 trampoline for PYBIND11_OVERRIDE dispatch.
//   xxxBridge*      — C-ABI callbacks that acquire the GIL, invoke virtual.
//   PyXxxOwner      — RAII over FloxXxxHandle; non-copyable.
//
// PyRunner / PyBacktestRunner own the Owner, holding both the C-ABI handle
// and a shared_ptr to the Python object so it survives the engine's
// non-owning use of `user_data`.

#pragma once

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/capi/flox_capi.h"
#include "flox/common.h"
#include "flox/execution/abstract_execution_listener.h"
#include "flox/execution/abstract_executor.h"
#include "flox/execution/exchange_capabilities.h"
#include "flox/execution/order.h"
#include "flox/killswitch/abstract_killswitch.h"
#include "flox/metrics/abstract_pnl_tracker.h"
#include "flox/risk/abstract_risk_manager.h"
#include "flox/validation/abstract_order_validator.h"
#include "types_bindings.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace flox_py
{

// ── Shared Py-side mirrors of C-ABI structs ─────────────────────────────

struct PyOrder
{
  uint64_t id{0};
  uint64_t client_order_id{0};
  uint32_t symbol{0};
  uint16_t strategy_id{0};
  uint16_t order_tag{0};
  std::string side;        // "buy" / "sell"
  std::string order_type;  // "market", "limit", ...
  std::string time_in_force;
  bool reduce_only{false};
  bool post_only{false};
  bool close_position{false};
  double price{0.0};
  double quantity{0.0};
  double filled_quantity{0.0};
  double trigger_price{0.0};
  double trailing_offset{0.0};
  int64_t created_at_ns{0};
  int64_t exchange_ts_ns{0};
};

inline PyOrder pyOrderFromC(const FloxOrder* o)
{
  static constexpr const char* kOrderTypes[] = {
      "limit", "market", "stop_market", "stop_limit",
      "tp_market", "tp_limit", "trailing_stop", "iceberg"};
  static constexpr const char* kTif[] = {"gtc", "ioc", "fok", "gtd", "post_only"};
  PyOrder po{};
  po.id = o->id;
  po.client_order_id = o->client_order_id;
  po.symbol = o->symbol;
  po.strategy_id = o->strategy_id;
  po.order_tag = o->order_tag;
  po.side = o->side == 0 ? "buy" : "sell";
  po.order_type = o->type < 8 ? kOrderTypes[o->type] : "unknown";
  po.time_in_force = o->time_in_force < 5 ? kTif[o->time_in_force] : "unknown";
  po.reduce_only = (o->flags & 0x01) != 0;
  po.close_position = (o->flags & 0x02) != 0;
  po.post_only = (o->flags & 0x04) != 0;
  po.price = static_cast<double>(o->price_raw) / 1e8;
  po.quantity = static_cast<double>(o->quantity_raw) / 1e8;
  po.filled_quantity = static_cast<double>(o->filled_quantity_raw) / 1e8;
  po.trigger_price = static_cast<double>(o->trigger_price_raw) / 1e8;
  po.trailing_offset = static_cast<double>(o->trailing_offset_raw) / 1e8;
  po.created_at_ns = o->created_at_ns;
  po.exchange_ts_ns = o->exchange_ts_ns;
  return po;
}

inline FloxOrder cOrderFromPy(const PyOrder& po)
{
  FloxOrder o{};
  o.id = po.id;
  o.client_order_id = po.client_order_id;
  o.symbol = po.symbol;
  o.strategy_id = po.strategy_id;
  o.order_tag = po.order_tag;
  o.side = po.side == "buy" ? 0u : 1u;
  if (po.order_type == "limit")
  {
    o.type = 0;
  }
  else if (po.order_type == "market")
  {
    o.type = 1;
  }
  else if (po.order_type == "stop_market")
  {
    o.type = 2;
  }
  else if (po.order_type == "stop_limit")
  {
    o.type = 3;
  }
  else if (po.order_type == "tp_market")
  {
    o.type = 4;
  }
  else if (po.order_type == "tp_limit")
  {
    o.type = 5;
  }
  else if (po.order_type == "trailing_stop")
  {
    o.type = 6;
  }
  else if (po.order_type == "iceberg")
  {
    o.type = 7;
  }
  if (po.time_in_force == "gtc")
  {
    o.time_in_force = 0;
  }
  else if (po.time_in_force == "ioc")
  {
    o.time_in_force = 1;
  }
  else if (po.time_in_force == "fok")
  {
    o.time_in_force = 2;
  }
  else if (po.time_in_force == "gtd")
  {
    o.time_in_force = 3;
  }
  else if (po.time_in_force == "post_only")
  {
    o.time_in_force = 4;
  }
  uint8_t flags = 0;
  if (po.reduce_only)
  {
    flags |= 0x01;
  }
  if (po.close_position)
  {
    flags |= 0x02;
  }
  if (po.post_only)
  {
    flags |= 0x04;
  }
  o.flags = flags;
  o.price_raw = static_cast<int64_t>(po.price * 1e8);
  o.quantity_raw = static_cast<int64_t>(po.quantity * 1e8);
  o.filled_quantity_raw = static_cast<int64_t>(po.filled_quantity * 1e8);
  o.trigger_price_raw = static_cast<int64_t>(po.trigger_price * 1e8);
  o.trailing_offset_raw = static_cast<int64_t>(po.trailing_offset * 1e8);
  o.created_at_ns = po.created_at_ns;
  o.exchange_ts_ns = po.exchange_ts_ns;
  return o;
}

struct PyExchangeCapabilities
{
  bool stop_market{false};
  bool stop_limit{false};
  bool take_profit_market{false};
  bool take_profit_limit{false};
  bool trailing_stop{false};
  bool iceberg{false};
  bool oco{false};
  bool gtc{true};
  bool ioc{true};
  bool fok{false};
  bool gtd{false};
  bool post_only{false};
  bool reduce_only{false};
  bool close_position{false};
};

inline FloxExchangeCapabilities cCapsFromPy(const PyExchangeCapabilities& p)
{
  FloxExchangeCapabilities c{};
  c.supports_stop_market = p.stop_market ? 1 : 0;
  c.supports_stop_limit = p.stop_limit ? 1 : 0;
  c.supports_take_profit_market = p.take_profit_market ? 1 : 0;
  c.supports_take_profit_limit = p.take_profit_limit ? 1 : 0;
  c.supports_trailing_stop = p.trailing_stop ? 1 : 0;
  c.supports_iceberg = p.iceberg ? 1 : 0;
  c.supports_oco = p.oco ? 1 : 0;
  c.supports_gtc = p.gtc ? 1 : 0;
  c.supports_ioc = p.ioc ? 1 : 0;
  c.supports_fok = p.fok ? 1 : 0;
  c.supports_gtd = p.gtd ? 1 : 0;
  c.supports_post_only = p.post_only ? 1 : 0;
  c.supports_reduce_only = p.reduce_only ? 1 : 0;
  c.supports_close_position = p.close_position ? 1 : 0;
  return c;
}

inline PyExchangeCapabilities pyCapsFromC(const FloxExchangeCapabilities& c)
{
  PyExchangeCapabilities p{};
  p.stop_market = c.supports_stop_market != 0;
  p.stop_limit = c.supports_stop_limit != 0;
  p.take_profit_market = c.supports_take_profit_market != 0;
  p.take_profit_limit = c.supports_take_profit_limit != 0;
  p.trailing_stop = c.supports_trailing_stop != 0;
  p.iceberg = c.supports_iceberg != 0;
  p.oco = c.supports_oco != 0;
  p.gtc = c.supports_gtc != 0;
  p.ioc = c.supports_ioc != 0;
  p.fok = c.supports_fok != 0;
  p.gtd = c.supports_gtd != 0;
  p.post_only = c.supports_post_only != 0;
  p.reduce_only = c.supports_reduce_only != 0;
  p.close_position = c.supports_close_position != 0;
  return p;
}

// ── Helper: invoke Python callback under GIL, swallow exceptions ────────
//
// Hooks fire from C-ABI consumer threads (live engine) or the publisher
// thread (sync runner). Either way we must hold the GIL while invoking
// virtual dispatch. Exceptions from Python are caught and printed —
// propagating them across the C ABI boundary is undefined behaviour.

template <typename Fn>
inline void invokeUnderGil(Fn fn)
{
  py::gil_scoped_acquire gil;
  try
  {
    fn();
  }
  catch (const py::error_already_set& e)
  {
    PyErr_Print();
  }
  catch (const std::exception& e)
  {
    PySys_WriteStderr("flox hook callback raised: %s\n", e.what());
  }
}

// ── PnLTracker hook ──────────────────────────────────────────────────────

class PyPnLTracker
{
 public:
  virtual ~PyPnLTracker() = default;
  virtual void on_signal(const PySignal& /*signal*/) {}
};

class PyPnLTrackerTrampoline : public PyPnLTracker
{
 public:
  using PyPnLTracker::PyPnLTracker;
  void on_signal(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(void, PyPnLTracker, on_signal, sig);
  }
};

inline void pnlOnSignalBridge(void* ud, const FloxSignal* sig)
{
  auto* py_obj = static_cast<PyPnLTracker*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_signal(pySignalFromC(sig)); });
}

class PyPnLTrackerOwner
{
 public:
  explicit PyPnLTrackerOwner(std::shared_ptr<PyPnLTracker> delegate)
      : _delegate(std::move(delegate))
  {
    FloxPnLTrackerCallbacks cb{};
    cb.on_signal = pnlOnSignalBridge;
    cb.user_data = _delegate.get();
    _handle = flox_pnl_tracker_create(cb);
  }
  ~PyPnLTrackerOwner()
  {
    if (_handle)
    {
      flox_pnl_tracker_destroy(_handle);
    }
  }
  PyPnLTrackerOwner(const PyPnLTrackerOwner&) = delete;
  PyPnLTrackerOwner& operator=(const PyPnLTrackerOwner&) = delete;
  FloxPnLTrackerHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyPnLTracker> _delegate;
  FloxPnLTrackerHandle _handle{nullptr};
};

// ── StorageSink hook ────────────────────────────────────────────────────

class PyStorageSink
{
 public:
  virtual ~PyStorageSink() = default;
  virtual void store(const PySignal& /*signal*/) {}
};

class PyStorageSinkTrampoline : public PyStorageSink
{
 public:
  using PyStorageSink::PyStorageSink;
  void store(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(void, PyStorageSink, store, sig);
  }
};

inline void storageStoreBridge(void* ud, const FloxSignal* sig)
{
  auto* py_obj = static_cast<PyStorageSink*>(ud);
  invokeUnderGil([&]
                 { py_obj->store(pySignalFromC(sig)); });
}

class PyStorageSinkOwner
{
 public:
  explicit PyStorageSinkOwner(std::shared_ptr<PyStorageSink> delegate)
      : _delegate(std::move(delegate))
  {
    FloxStorageSinkCallbacks cb{};
    cb.store = storageStoreBridge;
    cb.user_data = _delegate.get();
    _handle = flox_storage_sink_create(cb);
  }
  ~PyStorageSinkOwner()
  {
    if (_handle)
    {
      flox_storage_sink_destroy(_handle);
    }
  }
  PyStorageSinkOwner(const PyStorageSinkOwner&) = delete;
  PyStorageSinkOwner& operator=(const PyStorageSinkOwner&) = delete;
  FloxStorageSinkHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyStorageSink> _delegate;
  FloxStorageSinkHandle _handle{nullptr};
};

// ── RiskManager hook ────────────────────────────────────────────────────

class PyRiskManager
{
 public:
  virtual ~PyRiskManager() = default;
  // Return True to allow the signal, False to drop it.
  virtual bool allow(const PySignal& /*signal*/) { return true; }
};

class PyRiskManagerTrampoline : public PyRiskManager
{
 public:
  using PyRiskManager::PyRiskManager;
  bool allow(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(bool, PyRiskManager, allow, sig);
  }
};

inline uint8_t riskAllowBridge(void* ud, const FloxSignal* sig)
{
  auto* py_obj = static_cast<PyRiskManager*>(ud);
  uint8_t result = 1;
  invokeUnderGil([&]
                 { result = py_obj->allow(pySignalFromC(sig)) ? 1u : 0u; });
  return result;
}

class PyRiskManagerOwner
{
 public:
  explicit PyRiskManagerOwner(std::shared_ptr<PyRiskManager> delegate)
      : _delegate(std::move(delegate))
  {
    FloxRiskManagerCallbacks cb{};
    cb.allow = riskAllowBridge;
    cb.user_data = _delegate.get();
    _handle = flox_risk_manager_create(cb);
  }
  ~PyRiskManagerOwner()
  {
    if (_handle)
    {
      flox_risk_manager_destroy(_handle);
    }
  }
  PyRiskManagerOwner(const PyRiskManagerOwner&) = delete;
  PyRiskManagerOwner& operator=(const PyRiskManagerOwner&) = delete;
  FloxRiskManagerHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyRiskManager> _delegate;
  FloxRiskManagerHandle _handle{nullptr};
};

// ── KillSwitch hook ─────────────────────────────────────────────────────

class PyKillSwitch
{
 public:
  virtual ~PyKillSwitch() = default;
  // Return True to let the signal through, False to halt trading.
  virtual bool check(const PySignal& /*signal*/) { return true; }
};

class PyKillSwitchTrampoline : public PyKillSwitch
{
 public:
  using PyKillSwitch::PyKillSwitch;
  bool check(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(bool, PyKillSwitch, check, sig);
  }
};

inline uint8_t killCheckBridge(void* ud, const FloxSignal* sig)
{
  auto* py_obj = static_cast<PyKillSwitch*>(ud);
  uint8_t result = 1;
  invokeUnderGil([&]
                 { result = py_obj->check(pySignalFromC(sig)) ? 1u : 0u; });
  return result;
}

class PyKillSwitchOwner
{
 public:
  explicit PyKillSwitchOwner(std::shared_ptr<PyKillSwitch> delegate)
      : _delegate(std::move(delegate))
  {
    FloxKillSwitchCallbacks cb{};
    cb.check = killCheckBridge;
    cb.user_data = _delegate.get();
    _handle = flox_kill_switch_create(cb);
  }
  ~PyKillSwitchOwner()
  {
    if (_handle)
    {
      flox_kill_switch_destroy(_handle);
    }
  }
  PyKillSwitchOwner(const PyKillSwitchOwner&) = delete;
  PyKillSwitchOwner& operator=(const PyKillSwitchOwner&) = delete;
  FloxKillSwitchHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyKillSwitch> _delegate;
  FloxKillSwitchHandle _handle{nullptr};
};

// ── OrderValidator hook ─────────────────────────────────────────────────

class PyOrderValidator
{
 public:
  virtual ~PyOrderValidator() = default;
  virtual bool validate(const PySignal& /*signal*/) { return true; }
};

class PyOrderValidatorTrampoline : public PyOrderValidator
{
 public:
  using PyOrderValidator::PyOrderValidator;
  bool validate(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(bool, PyOrderValidator, validate, sig);
  }
};

inline uint8_t orderValidateBridge(void* ud, const FloxSignal* sig)
{
  auto* py_obj = static_cast<PyOrderValidator*>(ud);
  uint8_t result = 1;
  invokeUnderGil([&]
                 { result = py_obj->validate(pySignalFromC(sig)) ? 1u : 0u; });
  return result;
}

class PyOrderValidatorOwner
{
 public:
  explicit PyOrderValidatorOwner(std::shared_ptr<PyOrderValidator> delegate)
      : _delegate(std::move(delegate))
  {
    FloxOrderValidatorCallbacks cb{};
    cb.validate = orderValidateBridge;
    cb.user_data = _delegate.get();
    _handle = flox_order_validator_create(cb);
  }
  ~PyOrderValidatorOwner()
  {
    if (_handle)
    {
      flox_order_validator_destroy(_handle);
    }
  }
  PyOrderValidatorOwner(const PyOrderValidatorOwner&) = delete;
  PyOrderValidatorOwner& operator=(const PyOrderValidatorOwner&) = delete;
  FloxOrderValidatorHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyOrderValidator> _delegate;
  FloxOrderValidatorHandle _handle{nullptr};
};

// ── MarketDataRecorder hook ─────────────────────────────────────────────

class PyMarketDataRecorderHook
{
 public:
  virtual ~PyMarketDataRecorderHook() = default;
  virtual void on_trade(const PyTradeData& /*trade*/) {}
  virtual void on_book_update(uint32_t /*symbol*/, bool /*is_snapshot*/,
                              const std::vector<std::pair<double, double>>& /*bids*/,
                              const std::vector<std::pair<double, double>>& /*asks*/,
                              int64_t /*ts_ns*/) {}
  virtual void on_start() {}
  virtual void on_stop() {}
};

class PyMarketDataRecorderHookTrampoline : public PyMarketDataRecorderHook
{
 public:
  using PyMarketDataRecorderHook::PyMarketDataRecorderHook;
  void on_trade(const PyTradeData& t) override
  {
    PYBIND11_OVERRIDE(void, PyMarketDataRecorderHook, on_trade, t);
  }
  void on_book_update(uint32_t symbol, bool is_snapshot,
                      const std::vector<std::pair<double, double>>& bids,
                      const std::vector<std::pair<double, double>>& asks,
                      int64_t ts) override
  {
    PYBIND11_OVERRIDE(void, PyMarketDataRecorderHook, on_book_update,
                      symbol, is_snapshot, bids, asks, ts);
  }
  void on_start() override
  {
    PYBIND11_OVERRIDE(void, PyMarketDataRecorderHook, on_start);
  }
  void on_stop() override
  {
    PYBIND11_OVERRIDE(void, PyMarketDataRecorderHook, on_stop);
  }
};

inline void recOnTradeBridge(void* ud, const FloxTradeData* t)
{
  auto* py_obj = static_cast<PyMarketDataRecorderHook*>(ud);
  invokeUnderGil([&]
                 {
    PyTradeData pt{};
    pt.symbol = t->symbol;
    pt.price = static_cast<double>(t->price_raw) / 1e8;
    pt.quantity = static_cast<double>(t->quantity_raw) / 1e8;
    pt.is_buy = t->is_buy != 0;
    pt.exchange_ts_ns = t->exchange_ts_ns;
    py_obj->on_trade(pt); });
}

inline void recOnBookBridge(void* ud, uint32_t symbol, uint8_t is_snap,
                            const FloxBookLevel* bids, uint32_t n_bids,
                            const FloxBookLevel* asks, uint32_t n_asks,
                            int64_t ts)
{
  auto* py_obj = static_cast<PyMarketDataRecorderHook*>(ud);
  invokeUnderGil([&]
                 {
    std::vector<std::pair<double, double>> b, a;
    b.reserve(n_bids);
    a.reserve(n_asks);
    for (uint32_t i = 0; i < n_bids; ++i){
      b.emplace_back(static_cast<double>(bids[i].price_raw) / 1e8,
                     static_cast<double>(bids[i].quantity_raw) / 1e8);
}
    for (uint32_t i = 0; i < n_asks; ++i){
      a.emplace_back(static_cast<double>(asks[i].price_raw) / 1e8,
                     static_cast<double>(asks[i].quantity_raw) / 1e8);
}
    py_obj->on_book_update(symbol, is_snap != 0, b, a, ts); });
}

inline void recOnStartBridge(void* ud)
{
  auto* py_obj = static_cast<PyMarketDataRecorderHook*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_start(); });
}
inline void recOnStopBridge(void* ud)
{
  auto* py_obj = static_cast<PyMarketDataRecorderHook*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_stop(); });
}

class PyMarketDataRecorderHookOwner
{
 public:
  explicit PyMarketDataRecorderHookOwner(std::shared_ptr<PyMarketDataRecorderHook> delegate)
      : _delegate(std::move(delegate))
  {
    FloxMarketDataRecorderCallbacks cb{};
    cb.on_trade = recOnTradeBridge;
    cb.on_book_update = recOnBookBridge;
    cb.on_start = recOnStartBridge;
    cb.on_stop = recOnStopBridge;
    cb.user_data = _delegate.get();
    _handle = flox_market_data_recorder_create(cb);
  }
  ~PyMarketDataRecorderHookOwner()
  {
    if (_handle)
    {
      flox_market_data_recorder_destroy(_handle);
    }
  }
  PyMarketDataRecorderHookOwner(const PyMarketDataRecorderHookOwner&) = delete;
  PyMarketDataRecorderHookOwner& operator=(const PyMarketDataRecorderHookOwner&) = delete;
  FloxMarketDataRecorderHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyMarketDataRecorderHook> _delegate;
  FloxMarketDataRecorderHandle _handle{nullptr};
};

// ── ReplaySource hook ───────────────────────────────────────────────────

struct PyReplayEvent
{
  std::string type;  // "trade", "book_snapshot", "book_delta"
  int64_t timestamp_ns{0};
  // Trade
  uint32_t trade_symbol{0};
  bool trade_is_buy{false};
  double trade_price{0.0};
  double trade_quantity{0.0};
  // Book
  uint32_t book_symbol{0};
  std::vector<std::pair<double, double>> bids;
  std::vector<std::pair<double, double>> asks;
};

class PyReplaySource
{
 public:
  virtual ~PyReplaySource() = default;
  virtual void on_start() {}
  virtual void on_stop() {}
  virtual bool seek_to(int64_t /*ts_ns*/) { return false; }
  // Return None to signal end of stream.
  virtual std::optional<PyReplayEvent> next() { return std::nullopt; }
};

class PyReplaySourceTrampoline : public PyReplaySource
{
 public:
  using PyReplaySource::PyReplaySource;
  void on_start() override { PYBIND11_OVERRIDE(void, PyReplaySource, on_start); }
  void on_stop() override { PYBIND11_OVERRIDE(void, PyReplaySource, on_stop); }
  bool seek_to(int64_t ts) override
  {
    PYBIND11_OVERRIDE(bool, PyReplaySource, seek_to, ts);
  }
  std::optional<PyReplayEvent> next() override
  {
    PYBIND11_OVERRIDE(std::optional<PyReplayEvent>, PyReplaySource, next);
  }
};

// Storage for book level arrays — must outlive the `next()` callback so
// pointers remain valid until the engine consumes them.
struct PyReplaySourceBookBuffers
{
  std::vector<FloxBookLevel> bids;
  std::vector<FloxBookLevel> asks;
};

inline uint8_t replayNextBridge(void* ud, FloxReplayEvent* out)
{
  // Bind the buffers to the Python object so they survive across calls.
  // This works because the engine consumes each event before calling
  // next() again (single-threaded forEach contract).
  struct Bound : PyReplaySourceBookBuffers
  {
  };
  static thread_local Bound buf;

  auto* py_obj = static_cast<PyReplaySource*>(ud);
  uint8_t produced = 0;
  invokeUnderGil([&]
                 {
    auto opt = py_obj->next();
    if (!opt.has_value())
    {
      return;
    }
    const PyReplayEvent& ev = *opt;
    out->timestamp_ns = ev.timestamp_ns;
    if (ev.type == "trade")
    {
      out->type = 1;
      out->trade_symbol = ev.trade_symbol;
      out->trade_is_buy = ev.trade_is_buy ? 1 : 0;
      out->trade_price_raw = static_cast<int64_t>(ev.trade_price * 1e8);
      out->trade_quantity_raw = static_cast<int64_t>(ev.trade_quantity * 1e8);
      out->n_bids = 0;
      out->n_asks = 0;
      out->bids = nullptr;
      out->asks = nullptr;
    }
    else
    {
      out->type = (ev.type == "book_snapshot") ? 2 : 3;
      out->book_symbol = ev.book_symbol;
      buf.bids.clear();
      buf.asks.clear();
      buf.bids.reserve(ev.bids.size());
      buf.asks.reserve(ev.asks.size());
      for (const auto& [p, q] : ev.bids){
        buf.bids.push_back({static_cast<int64_t>(p * 1e8), static_cast<int64_t>(q * 1e8)});
}
      for (const auto& [p, q] : ev.asks){
        buf.asks.push_back({static_cast<int64_t>(p * 1e8), static_cast<int64_t>(q * 1e8)});
}
      out->n_bids = static_cast<uint32_t>(buf.bids.size());
      out->n_asks = static_cast<uint32_t>(buf.asks.size());
      out->bids = buf.bids.empty() ? nullptr : buf.bids.data();
      out->asks = buf.asks.empty() ? nullptr : buf.asks.data();
      out->trade_symbol = 0;
      out->trade_is_buy = 0;
      out->trade_price_raw = 0;
      out->trade_quantity_raw = 0;
    }
    produced = 1; });
  return produced;
}

inline void replayOnStartBridge(void* ud)
{
  auto* py_obj = static_cast<PyReplaySource*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_start(); });
}
inline void replayOnStopBridge(void* ud)
{
  auto* py_obj = static_cast<PyReplaySource*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_stop(); });
}
inline uint8_t replaySeekBridge(void* ud, int64_t ts)
{
  auto* py_obj = static_cast<PyReplaySource*>(ud);
  uint8_t result = 0;
  invokeUnderGil([&]
                 { result = py_obj->seek_to(ts) ? 1u : 0u; });
  return result;
}

class PyReplaySourceOwner
{
 public:
  explicit PyReplaySourceOwner(std::shared_ptr<PyReplaySource> delegate)
      : _delegate(std::move(delegate))
  {
    FloxReplaySourceCallbacks cb{};
    cb.on_start = replayOnStartBridge;
    cb.on_stop = replayOnStopBridge;
    cb.seek_to = replaySeekBridge;
    cb.next = replayNextBridge;
    cb.user_data = _delegate.get();
    _handle = flox_replay_source_create(cb);
  }
  ~PyReplaySourceOwner()
  {
    if (_handle)
    {
      flox_replay_source_destroy(_handle);
    }
  }
  PyReplaySourceOwner(const PyReplaySourceOwner&) = delete;
  PyReplaySourceOwner& operator=(const PyReplaySourceOwner&) = delete;
  FloxReplaySourceHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyReplaySource> _delegate;
  FloxReplaySourceHandle _handle{nullptr};
};

// ── ExecutionListener hook ──────────────────────────────────────────────

class PyExecutionListener
{
 public:
  virtual ~PyExecutionListener() = default;
  virtual void on_submitted(const PyOrder&) {}
  virtual void on_accepted(const PyOrder&) {}
  virtual void on_partially_filled(const PyOrder&, double /*fill_qty*/) {}
  virtual void on_filled(const PyOrder&) {}
  virtual void on_pending_cancel(const PyOrder&) {}
  virtual void on_canceled(const PyOrder&) {}
  virtual void on_expired(const PyOrder&) {}
  virtual void on_rejected(const PyOrder&, const std::string& /*reason*/) {}
  virtual void on_replaced(const PyOrder&, const PyOrder&) {}
  virtual void on_pending_trigger(const PyOrder&) {}
  virtual void on_triggered(const PyOrder&) {}
  virtual void on_trailing_stop_updated(const PyOrder&, double /*new_trigger*/) {}
};

class PyExecutionListenerTrampoline : public PyExecutionListener
{
 public:
  using PyExecutionListener::PyExecutionListener;

  void on_submitted(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_submitted, order);
  }
  void on_accepted(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_accepted, order);
  }
  void on_partially_filled(const PyOrder& order, double fill_qty) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_partially_filled, order, fill_qty);
  }
  void on_filled(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_filled, order);
  }
  void on_pending_cancel(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_pending_cancel, order);
  }
  void on_canceled(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_canceled, order);
  }
  void on_expired(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_expired, order);
  }
  void on_rejected(const PyOrder& order, const std::string& reason) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_rejected, order, reason);
  }
  void on_replaced(const PyOrder& old_order, const PyOrder& new_order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_replaced, old_order, new_order);
  }
  void on_pending_trigger(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_pending_trigger, order);
  }
  void on_triggered(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_triggered, order);
  }
  void on_trailing_stop_updated(const PyOrder& order, double new_trigger) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_trailing_stop_updated, order, new_trigger);
  }
};

#define _BRIDGE_ORDER_FN(NAME, METHOD)                        \
  inline void NAME(void* ud, const FloxOrder* o)              \
  {                                                           \
    auto* py_obj = static_cast<PyExecutionListener*>(ud);     \
    invokeUnderGil([&] { py_obj->METHOD(pyOrderFromC(o)); }); \
  }

_BRIDGE_ORDER_FN(execOnSubmittedBridge, on_submitted)
_BRIDGE_ORDER_FN(execOnAcceptedBridge, on_accepted)
_BRIDGE_ORDER_FN(execOnFilledBridge, on_filled)
_BRIDGE_ORDER_FN(execOnPendingCancelBridge, on_pending_cancel)
_BRIDGE_ORDER_FN(execOnCanceledBridge, on_canceled)
_BRIDGE_ORDER_FN(execOnExpiredBridge, on_expired)
_BRIDGE_ORDER_FN(execOnPendingTriggerBridge, on_pending_trigger)
_BRIDGE_ORDER_FN(execOnTriggeredBridge, on_triggered)
#undef _BRIDGE_ORDER_FN

inline void execOnPartialFillBridge(void* ud, const FloxOrder* o, int64_t fill_qty_raw)
{
  auto* py_obj = static_cast<PyExecutionListener*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_partially_filled(pyOrderFromC(o), static_cast<double>(fill_qty_raw) / 1e8); });
}

inline void execOnRejectedBridge(void* ud, const FloxOrder* o, const char* reason)
{
  auto* py_obj = static_cast<PyExecutionListener*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_rejected(pyOrderFromC(o), reason ? std::string(reason) : std::string{}); });
}

inline void execOnReplacedBridge(void* ud, const FloxOrder* old_o, const FloxOrder* new_o)
{
  auto* py_obj = static_cast<PyExecutionListener*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_replaced(pyOrderFromC(old_o), pyOrderFromC(new_o)); });
}

inline void execOnTrailingUpdateBridge(void* ud, const FloxOrder* o, int64_t new_trigger_raw)
{
  auto* py_obj = static_cast<PyExecutionListener*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_trailing_stop_updated(pyOrderFromC(o),
                                                    static_cast<double>(new_trigger_raw) / 1e8); });
}

class PyExecutionListenerOwner
{
 public:
  explicit PyExecutionListenerOwner(std::shared_ptr<PyExecutionListener> delegate)
      : _delegate(std::move(delegate))
  {
    FloxExecutionListenerCallbacks cb{};
    cb.on_submitted = execOnSubmittedBridge;
    cb.on_accepted = execOnAcceptedBridge;
    cb.on_partially_filled = execOnPartialFillBridge;
    cb.on_filled = execOnFilledBridge;
    cb.on_pending_cancel = execOnPendingCancelBridge;
    cb.on_canceled = execOnCanceledBridge;
    cb.on_expired = execOnExpiredBridge;
    cb.on_rejected = execOnRejectedBridge;
    cb.on_replaced = execOnReplacedBridge;
    cb.on_pending_trigger = execOnPendingTriggerBridge;
    cb.on_triggered = execOnTriggeredBridge;
    cb.on_trailing_stop_updated = execOnTrailingUpdateBridge;
    cb.user_data = _delegate.get();
    _handle = flox_execution_listener_create(cb);
  }
  ~PyExecutionListenerOwner()
  {
    if (_handle)
    {
      flox_execution_listener_destroy(_handle);
    }
  }
  PyExecutionListenerOwner(const PyExecutionListenerOwner&) = delete;
  PyExecutionListenerOwner& operator=(const PyExecutionListenerOwner&) = delete;
  FloxExecutionListenerHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyExecutionListener> _delegate;
  FloxExecutionListenerHandle _handle{nullptr};
};

// ── Executor hook ───────────────────────────────────────────────────────

class PyExecutor
{
 public:
  virtual ~PyExecutor() = default;
  virtual void submit(const PyOrder&) {}
  virtual void cancel(uint64_t /*order_id*/) {}
  virtual void cancel_all(uint32_t /*symbol*/) {}
  virtual void replace(uint64_t /*old_id*/, const PyOrder&) {}
  virtual void submit_oco(const PyOrder&, const PyOrder&) {}
  virtual PyExchangeCapabilities capabilities() { return {}; }
  virtual void on_start() {}
  virtual void on_stop() {}
};

class PyExecutorTrampoline : public PyExecutor
{
 public:
  using PyExecutor::PyExecutor;
  void submit(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutor, submit, order);
  }
  void cancel(uint64_t order_id) override
  {
    PYBIND11_OVERRIDE(void, PyExecutor, cancel, order_id);
  }
  void cancel_all(uint32_t symbol) override
  {
    PYBIND11_OVERRIDE(void, PyExecutor, cancel_all, symbol);
  }
  void replace(uint64_t old_id, const PyOrder& new_order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutor, replace, old_id, new_order);
  }
  void submit_oco(const PyOrder& order1, const PyOrder& order2) override
  {
    PYBIND11_OVERRIDE(void, PyExecutor, submit_oco, order1, order2);
  }
  PyExchangeCapabilities capabilities() override
  {
    PYBIND11_OVERRIDE(PyExchangeCapabilities, PyExecutor, capabilities);
  }
  void on_start() override { PYBIND11_OVERRIDE(void, PyExecutor, on_start); }
  void on_stop() override { PYBIND11_OVERRIDE(void, PyExecutor, on_stop); }
};

inline void execSubmitBridge(void* ud, const FloxOrder* o)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->submit(pyOrderFromC(o)); });
}
inline void execCancelBridge(void* ud, uint64_t id)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->cancel(id); });
}
inline void execCancelAllBridge(void* ud, uint32_t s)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->cancel_all(s); });
}
inline void execReplaceBridge(void* ud, uint64_t old_id, const FloxOrder* o)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->replace(old_id, pyOrderFromC(o)); });
}
inline void execSubmitOcoBridge(void* ud, const FloxOrder* a, const FloxOrder* b)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->submit_oco(pyOrderFromC(a), pyOrderFromC(b)); });
}
inline void execCapsBridge(void* ud, FloxExchangeCapabilities* out)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { *out = cCapsFromPy(py_obj->capabilities()); });
}
inline void execorOnStartBridge(void* ud)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_start(); });
}
inline void execorOnStopBridge(void* ud)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_stop(); });
}

class PyExecutorOwner
{
 public:
  explicit PyExecutorOwner(std::shared_ptr<PyExecutor> delegate)
      : _delegate(std::move(delegate))
  {
    FloxExecutorCallbacks cb{};
    cb.submit = execSubmitBridge;
    cb.cancel = execCancelBridge;
    cb.cancel_all = execCancelAllBridge;
    cb.replace = execReplaceBridge;
    cb.submit_oco = execSubmitOcoBridge;
    cb.capabilities = execCapsBridge;
    cb.on_start = execorOnStartBridge;
    cb.on_stop = execorOnStopBridge;
    cb.user_data = _delegate.get();
    _handle = flox_executor_create(cb);
  }
  ~PyExecutorOwner()
  {
    if (_handle)
    {
      flox_executor_destroy(_handle);
    }
  }
  PyExecutorOwner(const PyExecutorOwner&) = delete;
  PyExecutorOwner& operator=(const PyExecutorOwner&) = delete;
  FloxExecutorHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyExecutor> _delegate;
  FloxExecutorHandle _handle{nullptr};
};

// ── Logger callback ─────────────────────────────────────────────────────

inline py::object& globalLogCallback()
{
  static py::object cb;
  return cb;
}

inline void loggerBridge(void* /*ud*/, int32_t level, const char* msg)
{
  invokeUnderGil([&]
                 {
    auto& cb = globalLogCallback();
    if (!cb.is_none())
    {
      cb(level, msg ? std::string(msg) : std::string{});
    } });
}

inline void setPythonLogCallback(py::object cb)
{
  if (cb.is_none())
  {
    flox_set_log_callback(nullptr, nullptr);
    globalLogCallback() = py::none();
  }
  else
  {
    globalLogCallback() = std::move(cb);
    flox_set_log_callback(loggerBridge, nullptr);
  }
}

// ── Direct-C++ adapters for BacktestRunner ──────────────────────────────
//
// PyBacktestRunner currently wraps flox::BacktestRunner directly (no C-ABI
// indirection). Its setExecutor / addExecutionListener methods take
// flox::IOrderExecutor* and flox::IOrderExecutionListener* — so we bridge
// the Pythonic delegate to those C++ interfaces here, mirroring the
// CapiExecutor / CapiExecutionListener adapters that live in
// src/capi/flox_capi.cpp.

namespace cxx_adapters
{

inline flox::Order pyOrderToFlox(const PyOrder& po)
{
  flox::Order o{};
  o.id = po.id;
  o.clientOrderId = po.client_order_id;
  o.symbol = po.symbol;
  o.strategyId = po.strategy_id;
  o.orderTag = po.order_tag;
  o.side = (po.side == "buy") ? flox::Side::BUY : flox::Side::SELL;
  if (po.order_type == "limit")
  {
    o.type = flox::OrderType::LIMIT;
  }
  else if (po.order_type == "market")
  {
    o.type = flox::OrderType::MARKET;
  }
  else if (po.order_type == "stop_market")
  {
    o.type = flox::OrderType::STOP_MARKET;
  }
  else if (po.order_type == "stop_limit")
  {
    o.type = flox::OrderType::STOP_LIMIT;
  }
  else if (po.order_type == "tp_market")
  {
    o.type = flox::OrderType::TAKE_PROFIT_MARKET;
  }
  else if (po.order_type == "tp_limit")
  {
    o.type = flox::OrderType::TAKE_PROFIT_LIMIT;
  }
  else if (po.order_type == "trailing_stop")
  {
    o.type = flox::OrderType::TRAILING_STOP;
  }
  else if (po.order_type == "iceberg")
  {
    o.type = flox::OrderType::ICEBERG;
  }
  if (po.time_in_force == "ioc")
  {
    o.timeInForce = flox::TimeInForce::IOC;
  }
  else if (po.time_in_force == "fok")
  {
    o.timeInForce = flox::TimeInForce::FOK;
  }
  else if (po.time_in_force == "gtd")
  {
    o.timeInForce = flox::TimeInForce::GTD;
  }
  else if (po.time_in_force == "post_only")
  {
    o.timeInForce = flox::TimeInForce::POST_ONLY;
  }
  else
  {
    o.timeInForce = flox::TimeInForce::GTC;
  }
  o.flags.reduceOnly = po.reduce_only ? 1 : 0;
  o.flags.closePosition = po.close_position ? 1 : 0;
  o.flags.postOnly = po.post_only ? 1 : 0;
  o.price = flox::Price::fromDouble(po.price);
  o.quantity = flox::Quantity::fromDouble(po.quantity);
  o.filledQuantity = flox::Quantity::fromDouble(po.filled_quantity);
  o.triggerPrice = flox::Price::fromDouble(po.trigger_price);
  o.trailingOffset = flox::Price::fromDouble(po.trailing_offset);
  return o;
}

inline PyOrder floxOrderToPy(const flox::Order& o)
{
  PyOrder po{};
  po.id = o.id;
  po.client_order_id = o.clientOrderId;
  po.symbol = o.symbol;
  po.strategy_id = o.strategyId;
  po.order_tag = o.orderTag;
  po.side = (o.side == flox::Side::BUY) ? "buy" : "sell";
  switch (o.type)
  {
    case flox::OrderType::LIMIT:
      po.order_type = "limit";
      break;
    case flox::OrderType::MARKET:
      po.order_type = "market";
      break;
    case flox::OrderType::STOP_MARKET:
      po.order_type = "stop_market";
      break;
    case flox::OrderType::STOP_LIMIT:
      po.order_type = "stop_limit";
      break;
    case flox::OrderType::TAKE_PROFIT_MARKET:
      po.order_type = "tp_market";
      break;
    case flox::OrderType::TAKE_PROFIT_LIMIT:
      po.order_type = "tp_limit";
      break;
    case flox::OrderType::TRAILING_STOP:
      po.order_type = "trailing_stop";
      break;
    case flox::OrderType::ICEBERG:
      po.order_type = "iceberg";
      break;
  }
  switch (o.timeInForce)
  {
    case flox::TimeInForce::GTC:
      po.time_in_force = "gtc";
      break;
    case flox::TimeInForce::IOC:
      po.time_in_force = "ioc";
      break;
    case flox::TimeInForce::FOK:
      po.time_in_force = "fok";
      break;
    case flox::TimeInForce::GTD:
      po.time_in_force = "gtd";
      break;
    case flox::TimeInForce::POST_ONLY:
      po.time_in_force = "post_only";
      break;
  }
  po.reduce_only = o.flags.reduceOnly != 0;
  po.close_position = o.flags.closePosition != 0;
  po.post_only = o.flags.postOnly != 0;
  po.price = o.price.toDouble();
  po.quantity = o.quantity.toDouble();
  po.filled_quantity = o.filledQuantity.toDouble();
  po.trigger_price = o.triggerPrice.toDouble();
  po.trailing_offset = o.trailingOffset.toDouble();
  return po;
}

class PyExecutorCxxAdapter : public flox::IOrderExecutor
{
 public:
  explicit PyExecutorCxxAdapter(std::shared_ptr<PyExecutor> delegate)
      : _delegate(std::move(delegate))
  {
  }
  void start() override
  {
    invokeUnderGil([&]
                   { _delegate->on_start(); });
  }
  void stop() override
  {
    invokeUnderGil([&]
                   { _delegate->on_stop(); });
  }
  void submitOrder(const flox::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->submit(floxOrderToPy(o)); });
  }
  void cancelOrder(flox::OrderId id) override
  {
    invokeUnderGil([&]
                   { _delegate->cancel(static_cast<uint64_t>(id)); });
  }
  void cancelAllOrders(flox::SymbolId sym) override
  {
    invokeUnderGil([&]
                   { _delegate->cancel_all(static_cast<uint32_t>(sym)); });
  }
  void replaceOrder(flox::OrderId old_id, const flox::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->replace(static_cast<uint64_t>(old_id), floxOrderToPy(o)); });
  }
  void submitOCO(const flox::OCOParams& p) override
  {
    invokeUnderGil([&]
                   { _delegate->submit_oco(floxOrderToPy(p.order1), floxOrderToPy(p.order2)); });
  }
  flox::ExchangeCapabilities capabilities() const override
  {
    flox::ExchangeCapabilities out{};
    invokeUnderGil([&]
                   {
      auto pc = _delegate->capabilities();
      out.supportsStopMarket = pc.stop_market;
      out.supportsStopLimit = pc.stop_limit;
      out.supportsTakeProfitMarket = pc.take_profit_market;
      out.supportsTakeProfitLimit = pc.take_profit_limit;
      out.supportsTrailingStop = pc.trailing_stop;
      out.supportsIceberg = pc.iceberg;
      out.supportsOCO = pc.oco;
      out.supportsGTC = pc.gtc;
      out.supportsIOC = pc.ioc;
      out.supportsFOK = pc.fok;
      out.supportsGTD = pc.gtd;
      out.supportsPostOnly = pc.post_only;
      out.supportsReduceOnly = pc.reduce_only;
      out.supportsClosePosition = pc.close_position; });
    return out;
  }

 private:
  std::shared_ptr<PyExecutor> _delegate;
};

class PyExecutionListenerCxxAdapter : public flox::IOrderExecutionListener
{
 public:
  PyExecutionListenerCxxAdapter(flox::SubscriberId id,
                                std::shared_ptr<PyExecutionListener> delegate)
      : flox::IOrderExecutionListener(id), _delegate(std::move(delegate))
  {
  }
  void onOrderSubmitted(const flox::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_submitted(floxOrderToPy(o)); });
  }
  void onOrderAccepted(const flox::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_accepted(floxOrderToPy(o)); });
  }
  void onOrderFilled(const flox::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_filled(floxOrderToPy(o)); });
  }
  void onOrderPendingCancel(const flox::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_pending_cancel(floxOrderToPy(o)); });
  }
  void onOrderCanceled(const flox::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_canceled(floxOrderToPy(o)); });
  }
  void onOrderExpired(const flox::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_expired(floxOrderToPy(o)); });
  }
  void onOrderPendingTrigger(const flox::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_pending_trigger(floxOrderToPy(o)); });
  }
  void onOrderTriggered(const flox::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_triggered(floxOrderToPy(o)); });
  }

  void onOrderPartiallyFilled(const flox::Order& o, flox::Quantity q) override
  {
    invokeUnderGil([&]
                   { _delegate->on_partially_filled(floxOrderToPy(o), q.toDouble()); });
  }
  void onOrderRejected(const flox::Order& o, const std::string& reason) override
  {
    invokeUnderGil([&]
                   { _delegate->on_rejected(floxOrderToPy(o), reason); });
  }
  void onOrderReplaced(const flox::Order& a, const flox::Order& b) override
  {
    invokeUnderGil([&]
                   { _delegate->on_replaced(floxOrderToPy(a), floxOrderToPy(b)); });
  }
  void onTrailingStopUpdated(const flox::Order& o, flox::Price t) override
  {
    invokeUnderGil([&]
                   { _delegate->on_trailing_stop_updated(floxOrderToPy(o), t.toDouble()); });
  }

 private:
  std::shared_ptr<PyExecutionListener> _delegate;
};

// PyExecutionListenerCxxAdapter doesn't override on_submitted etc. — it
// uses the C++ method names. The Python class uses on_submitted,
// on_accepted, ..., which the adapter forwards to via the _BR1 macros.
// (Mapping is one-to-one: onOrderSubmitted → on_submitted, etc., done in
// each macro instance.)

// ── Pre-trade gate adapters (W1-T036) ────────────────────────────────
//
// Bridge the existing PyRiskManager / PyKillSwitch / PyOrderValidator /
// PyPnLTracker (which work on PySignal) into the engine-side
// IRiskManager / IKillSwitch / IOrderValidator / IPnLTracker
// interfaces (which work on Order). Conversion is one-shot per call —
// orders ship the same fields as signals plus side/type as Order
// enums, so we re-emit a PySignal shape so existing user code keeps
// working unchanged.

inline PySignal pySignalFromOrder(const flox::Order& o)
{
  PySignal ps{};
  ps.order_id = o.id;
  ps.symbol = o.symbol;
  ps.side = (o.side == flox::Side::BUY) ? "buy" : "sell";
  switch (o.type)
  {
    case flox::OrderType::LIMIT:
      ps.order_type = "limit";
      break;
    case flox::OrderType::MARKET:
      ps.order_type = "market";
      break;
    case flox::OrderType::STOP_MARKET:
      ps.order_type = "stop_market";
      break;
    case flox::OrderType::STOP_LIMIT:
      ps.order_type = "stop_limit";
      break;
    case flox::OrderType::TAKE_PROFIT_MARKET:
      ps.order_type = "tp_market";
      break;
    case flox::OrderType::TAKE_PROFIT_LIMIT:
      ps.order_type = "tp_limit";
      break;
    case flox::OrderType::TRAILING_STOP:
      ps.order_type = "trailing_stop";
      break;
    case flox::OrderType::ICEBERG:
      ps.order_type = "iceberg";
      break;
  }
  ps.price = o.price.toDouble();
  ps.quantity = o.quantity.toDouble();
  ps.trigger_price = o.triggerPrice.toDouble();
  ps.trailing_offset = o.trailingOffset.toDouble();
  return ps;
}

class PyRiskManagerCxxAdapter : public flox::IRiskManager
{
 public:
  explicit PyRiskManagerCxxAdapter(std::shared_ptr<PyRiskManager> delegate)
      : _delegate(std::move(delegate))
  {
  }
  bool allow(const flox::Order& order) const override
  {
    bool result = true;
    invokeUnderGil([&]
                   { result = _delegate->allow(pySignalFromOrder(order)); });
    return result;
  }

 private:
  std::shared_ptr<PyRiskManager> _delegate;
};

class PyKillSwitchCxxAdapter : public flox::IKillSwitch
{
 public:
  explicit PyKillSwitchCxxAdapter(std::shared_ptr<PyKillSwitch> delegate)
      : _delegate(std::move(delegate))
  {
  }
  // Engine contract: check(order) may flip state; isTriggered() reads
  // it. The Python check() returns True=allow / False=halt, so we
  // store its inverse as `_triggered`.
  void check(const flox::Order& order) override
  {
    bool allowed = true;
    invokeUnderGil([&]
                   { allowed = _delegate->check(pySignalFromOrder(order)); });
    if (!allowed)
    {
      _triggered = true;
    }
  }
  void trigger(const std::string& reason) override
  {
    _triggered = true;
    _reason = reason;
  }
  bool isTriggered() const override { return _triggered; }
  std::string reason() const override { return _reason; }

 private:
  std::shared_ptr<PyKillSwitch> _delegate;
  bool _triggered{false};
  std::string _reason;
};

class PyOrderValidatorCxxAdapter : public flox::IOrderValidator
{
 public:
  explicit PyOrderValidatorCxxAdapter(std::shared_ptr<PyOrderValidator> delegate)
      : _delegate(std::move(delegate))
  {
  }
  // The Python validator returns True=valid / False=reject. We do not
  // expose a reason channel — bindings that want richer messaging
  // can raise via the engine's REJECTED order event.
  bool validate(const flox::Order& order, std::string& /*reason*/) const override
  {
    bool valid = true;
    invokeUnderGil([&]
                   { valid = _delegate->validate(pySignalFromOrder(order)); });
    return valid;
  }

 private:
  std::shared_ptr<PyOrderValidator> _delegate;
};

class PyPnLTrackerCxxAdapter : public flox::IPnLTracker
{
 public:
  explicit PyPnLTrackerCxxAdapter(std::shared_ptr<PyPnLTracker> delegate)
      : _delegate(std::move(delegate))
  {
  }
  void onOrderFilled(const flox::Order& order) override
  {
    invokeUnderGil([&]
                   { _delegate->on_signal(pySignalFromOrder(order)); });
  }

 private:
  std::shared_ptr<PyPnLTracker> _delegate;
};

}  // namespace cxx_adapters

}  // namespace flox_py
