/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// NAPI wrappers for the C-ABI extension hooks.
//
// API shape (idiomatic Node):
//
//   runner.setPnlTracker({ onSignal(sig) { ... } });
//   runner.setExecutor({
//     submit(order) { broker.place(order); },
//     cancel(orderId) { broker.cancel(orderId); },
//     capabilities() { return { stopMarket: true, oco: true }; },
//   });
//
// Each hook host:
//   - extracts named function references from the JS object on attach;
//   - derives from TsfnHost for cross-thread invocation in HookMode::Threaded
//     (LiveEngine consumer threads can't touch V8 directly);
//   - owns a Flox<Hook>Handle via RAII; non-copyable.
//
// On the lifetime of the threaded halves, see node/src/tsfn_util.h. A hook
// host's channel is scoped to the host rather than to a run, because a hook is
// attached and detached by the caller rather than by start()/stop().
//
// Note which of these paths runs today: nothing constructs a hook host with
// HookMode::Threaded. Every hook a Runner exposes is reachable only from the
// JS thread -- signals originate in the emitter handed to a strategy callback,
// which the strategy's own channel has already delivered onto that thread --
// so the Sync branch is the one that fires, and the queued branches below are
// kept correct rather than exercised.

#pragma once

#include <napi.h>

#include "flox/capi/flox_capi.h"
#include "flox/capi/order_type_names.hpp"
#include "tsfn_util.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace flox_node
{

// ── Helpers ─────────────────────────────────────────────────────────────

inline Napi::FunctionReference takeFn(Napi::Object obj, const char* name)
{
  auto val = obj.Get(name);
  if (val.IsFunction())
  {
    return Napi::Persistent(val.As<Napi::Function>());
  }
  return {};
}

// Build a JS object mirroring FloxSignal for the user callback.
inline Napi::Object signalToJs(Napi::Env env, const FloxSignal* s)
{
  auto obj = Napi::Object::New(env);
  obj.Set("orderId", Napi::Number::New(env, static_cast<double>(s->order_id)));
  obj.Set("symbol", Napi::Number::New(env, s->symbol));
  obj.Set("side", Napi::String::New(env, s->side == 0 ? "buy" : "sell"));
  obj.Set("orderType", Napi::String::New(env, flox::capi::signalTypeName(s->order_type)));
  obj.Set("price", Napi::Number::New(env, s->price));
  obj.Set("quantity", Napi::Number::New(env, s->quantity));
  obj.Set("triggerPrice", Napi::Number::New(env, s->trigger_price));
  obj.Set("trailingOffset", Napi::Number::New(env, s->trailing_offset));
  obj.Set("trailingBps", Napi::Number::New(env, s->trailing_bps));
  obj.Set("newPrice", Napi::Number::New(env, s->new_price));
  obj.Set("newQuantity", Napi::Number::New(env, s->new_quantity));
  obj.Set("rangeLower", Napi::Number::New(env, s->range_lower));
  obj.Set("rangeUpper", Napi::Number::New(env, s->range_upper));
  obj.Set("liquidity", Napi::Number::New(env, s->liquidity));
  return obj;
}

inline Napi::Object orderToJs(Napi::Env env, const FloxOrder* o)
{
  static constexpr const char* kTif[] = {"gtc", "ioc", "fok", "gtd", "post_only"};
  auto obj = Napi::Object::New(env);
  obj.Set("id", Napi::Number::New(env, static_cast<double>(o->id)));
  obj.Set("clientOrderId", Napi::Number::New(env, static_cast<double>(o->client_order_id)));
  obj.Set("symbol", Napi::Number::New(env, o->symbol));
  obj.Set("strategyId", Napi::Number::New(env, o->strategy_id));
  obj.Set("orderTag", Napi::Number::New(env, o->order_tag));
  obj.Set("side", Napi::String::New(env, o->side == 0 ? "buy" : "sell"));
  obj.Set("orderType", Napi::String::New(env, flox::capi::orderTypeNameLower(o->type)));
  obj.Set("timeInForce",
          Napi::String::New(env, o->time_in_force < 5 ? kTif[o->time_in_force] : "unknown"));
  obj.Set("reduceOnly", Napi::Boolean::New(env, (o->flags & 0x01) != 0));
  obj.Set("closePosition", Napi::Boolean::New(env, (o->flags & 0x02) != 0));
  obj.Set("postOnly", Napi::Boolean::New(env, (o->flags & 0x04) != 0));
  obj.Set("price", Napi::Number::New(env, o->price_raw / 1e8));
  obj.Set("quantity", Napi::Number::New(env, o->quantity_raw / 1e8));
  obj.Set("filledQuantity", Napi::Number::New(env, o->filled_quantity_raw / 1e8));
  obj.Set("triggerPrice", Napi::Number::New(env, o->trigger_price_raw / 1e8));
  obj.Set("trailingOffset", Napi::Number::New(env, o->trailing_offset_raw / 1e8));
  obj.Set("createdAtNs", Napi::Number::New(env, static_cast<double>(o->created_at_ns)));
  obj.Set("exchangeTsNs", Napi::Number::New(env, static_cast<double>(o->exchange_ts_ns)));
  return obj;
}

inline Napi::Object tradeToJs(Napi::Env env, const FloxTradeData* t)
{
  auto obj = Napi::Object::New(env);
  obj.Set("symbol", Napi::Number::New(env, t->symbol));
  obj.Set("price", Napi::Number::New(env, t->price_raw / 1e8));
  obj.Set("quantity", Napi::Number::New(env, t->quantity_raw / 1e8));
  obj.Set("isBuy", Napi::Boolean::New(env, t->is_buy != 0));
  obj.Set("exchangeTsNs", Napi::Number::New(env, static_cast<double>(t->exchange_ts_ns)));
  return obj;
}

inline Napi::Array bookLevelsToJs(Napi::Env env, const FloxBookLevel* lvls, uint32_t n)
{
  auto arr = Napi::Array::New(env, n);
  for (uint32_t i = 0; i < n; ++i)
  {
    auto pair = Napi::Array::New(env, 2);
    pair.Set(uint32_t{0}, Napi::Number::New(env, lvls[i].price_raw / 1e8));
    pair.Set(uint32_t{1}, Napi::Number::New(env, lvls[i].quantity_raw / 1e8));
    arr.Set(i, pair);
  }
  return arr;
}

// Flat BigInt64Array view of FloxBookLevel[]: [price_raw, qty_raw, ...].
// FloxBookLevel layout is two int64s, so we copy via a backing ArrayBuffer
// to keep ownership simple — the JS caller can read raw int64 ticks without
// the precision loss of /1e8 doubles.
inline Napi::BigInt64Array bookLevelsToBigInt64(Napi::Env env, const FloxBookLevel* lvls,
                                                uint32_t n)
{
  size_t elems = static_cast<size_t>(n) * 2;
  auto buf = Napi::ArrayBuffer::New(env, elems * sizeof(int64_t));
  if (n > 0)
  {
    std::memcpy(buf.Data(), lvls, n * sizeof(FloxBookLevel));
  }
  return Napi::BigInt64Array::New(env, elems, buf, 0);
}

// Hook host mode:
//   Sync: callbacks fire on the JS thread (synchronous Runner /
//         BacktestRunner). Use direct Napi::FunctionReference::Call —
//         no TSF queuing, so the result is observable immediately on
//         return from runner.onTrade / runOhlcv.
//   Threaded: callbacks fire from a C++ consumer thread (LiveEngine).
//             Must go through Napi::ThreadSafeFunction; the JS handler
//             runs at the next Node event loop tick.
enum class HookMode
{
  Sync,
  Threaded
};

// ── PnLTracker ──────────────────────────────────────────────────────────

struct PnLTrackerHost : TsfnHost
{
  Napi::FunctionReference on_signal_fn;
  HookMode mode;
  Napi::Env env;
  FloxPnLTrackerHandle handle{nullptr};

  PnLTrackerHost(Napi::Env env_, Napi::Object obj, HookMode m = HookMode::Sync)
      : TsfnHost(env_), on_signal_fn(takeFn(obj, "onSignal")), mode(m), env(env_)
  {
    if (mode == HookMode::Threaded)
    {
      openChannel("flox_pnl_cb");
    }
    FloxPnLTrackerCallbacks cb{};
    cb.on_signal = &PnLTrackerHost::onSignalBridge;
    cb.user_data = this;
    handle = flox_pnl_tracker_create(cb);
  }
  ~PnLTrackerHost()
  {
    if (handle)
    {
      flox_pnl_tracker_destroy(handle);
    }
  }
  PnLTrackerHost(const PnLTrackerHost&) = delete;
  PnLTrackerHost& operator=(const PnLTrackerHost&) = delete;

  static void onSignalBridge(void* ud, const FloxSignal* sig)
  {
    auto* self = static_cast<PnLTrackerHost*>(ud);
    if (self->on_signal_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->on_signal_fn.Call({signalToJs(self->env, sig)});
      return;
    }
    auto* sig_copy = new FloxSignal(*sig);
    self->post(sig_copy,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, FloxSignal* s)
               {
                 std::unique_ptr<FloxSignal> guard(s);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_signal_fn.Call({signalToJs(env, s)});
               });
  }
};

// ── StorageSink ─────────────────────────────────────────────────────────

struct StorageSinkHost : TsfnHost
{
  Napi::FunctionReference store_fn;
  HookMode mode;
  Napi::Env env;
  FloxStorageSinkHandle handle{nullptr};

  StorageSinkHost(Napi::Env env_, Napi::Object obj, HookMode m = HookMode::Sync)
      : TsfnHost(env_), store_fn(takeFn(obj, "store")), mode(m), env(env_)
  {
    if (mode == HookMode::Threaded)
    {
      openChannel("flox_storage_cb");
    }
    FloxStorageSinkCallbacks cb{};
    cb.store = &StorageSinkHost::storeBridge;
    cb.user_data = this;
    handle = flox_storage_sink_create(cb);
  }
  ~StorageSinkHost()
  {
    if (handle)
    {
      flox_storage_sink_destroy(handle);
    }
  }
  StorageSinkHost(const StorageSinkHost&) = delete;
  StorageSinkHost& operator=(const StorageSinkHost&) = delete;

  static void storeBridge(void* ud, const FloxSignal* sig)
  {
    auto* self = static_cast<StorageSinkHost*>(ud);
    if (self->store_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->store_fn.Call({signalToJs(self->env, sig)});
      return;
    }
    auto* sig_copy = new FloxSignal(*sig);
    self->post(sig_copy,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, FloxSignal* s)
               {
                 std::unique_ptr<FloxSignal> guard(s);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->store_fn.Call({signalToJs(env, s)});
               });
  }
};

// ── RiskManager / KillSwitch / OrderValidator (gate hooks) ──────────────
//
// These are pre-trade gates: return false → drop the signal. We must
// block on the JS callback for the result, which is incompatible with
// LiveEngine's async consumer thread (would deadlock). Document this
// limitation: gate hooks only work with the synchronous Runner.

template <typename Callbacks, FloxRiskManagerHandle (*Create)(Callbacks),
          void (*Destroy)(FloxRiskManagerHandle)>
struct GateHostBase
{
  Napi::FunctionReference fn;
  FloxRiskManagerHandle handle{nullptr};
  Napi::Env env;

  GateHostBase(Napi::Env env_, Napi::Object obj, const char* fn_name)
      : fn(takeFn(obj, fn_name)), env(env_)
  {
  }
  ~GateHostBase()
  {
    if (handle)
    {
      Destroy(handle);
    }
  }
  GateHostBase(const GateHostBase&) = delete;
  GateHostBase& operator=(const GateHostBase&) = delete;
};

struct RiskManagerHost
{
  Napi::FunctionReference allow_fn;
  Napi::Env env;
  FloxRiskManagerHandle handle{nullptr};

  RiskManagerHost(Napi::Env env_, Napi::Object obj)
      : allow_fn(takeFn(obj, "allow")), env(env_)
  {
    FloxRiskManagerCallbacks cb{};
    cb.allow = &RiskManagerHost::allowBridge;
    cb.user_data = this;
    handle = flox_risk_manager_create(cb);
  }
  ~RiskManagerHost()
  {
    if (handle)
    {
      flox_risk_manager_destroy(handle);
    }
  }
  RiskManagerHost(const RiskManagerHost&) = delete;
  RiskManagerHost& operator=(const RiskManagerHost&) = delete;

  static uint8_t allowBridge(void* ud, const FloxSignal* sig)
  {
    auto* self = static_cast<RiskManagerHost*>(ud);
    if (self->allow_fn.IsEmpty())
    {
      return 1;
    }
    // Synchronous — only safe to call from the JS thread (sync Runner).
    auto result = self->allow_fn.Call({signalToJs(self->env, sig)});
    return (result.IsBoolean() && result.As<Napi::Boolean>().Value()) ? 1u : 0u;
  }
};

struct KillSwitchHost
{
  Napi::FunctionReference check_fn;
  Napi::Env env;
  FloxKillSwitchHandle handle{nullptr};

  KillSwitchHost(Napi::Env env_, Napi::Object obj)
      : check_fn(takeFn(obj, "check")), env(env_)
  {
    FloxKillSwitchCallbacks cb{};
    cb.check = &KillSwitchHost::checkBridge;
    cb.user_data = this;
    handle = flox_kill_switch_create(cb);
  }
  ~KillSwitchHost()
  {
    if (handle)
    {
      flox_kill_switch_destroy(handle);
    }
  }
  KillSwitchHost(const KillSwitchHost&) = delete;
  KillSwitchHost& operator=(const KillSwitchHost&) = delete;

  static uint8_t checkBridge(void* ud, const FloxSignal* sig)
  {
    auto* self = static_cast<KillSwitchHost*>(ud);
    if (self->check_fn.IsEmpty())
    {
      return 1;
    }
    auto result = self->check_fn.Call({signalToJs(self->env, sig)});
    return (result.IsBoolean() && result.As<Napi::Boolean>().Value()) ? 1u : 0u;
  }
};

struct OrderValidatorHost
{
  Napi::FunctionReference validate_fn;
  Napi::Env env;
  FloxOrderValidatorHandle handle{nullptr};

  OrderValidatorHost(Napi::Env env_, Napi::Object obj)
      : validate_fn(takeFn(obj, "validate")), env(env_)
  {
    FloxOrderValidatorCallbacks cb{};
    cb.validate = &OrderValidatorHost::validateBridge;
    cb.user_data = this;
    handle = flox_order_validator_create(cb);
  }
  ~OrderValidatorHost()
  {
    if (handle)
    {
      flox_order_validator_destroy(handle);
    }
  }
  OrderValidatorHost(const OrderValidatorHost&) = delete;
  OrderValidatorHost& operator=(const OrderValidatorHost&) = delete;

  static uint8_t validateBridge(void* ud, const FloxSignal* sig)
  {
    auto* self = static_cast<OrderValidatorHost*>(ud);
    if (self->validate_fn.IsEmpty())
    {
      return 1;
    }
    auto result = self->validate_fn.Call({signalToJs(self->env, sig)});
    return (result.IsBoolean() && result.As<Napi::Boolean>().Value()) ? 1u : 0u;
  }
};

// ── MarketDataRecorderHook ──────────────────────────────────────────────

struct MarketDataRecorderHookHost : TsfnHost
{
  Napi::FunctionReference on_trade_fn;
  Napi::FunctionReference on_book_fn;
  Napi::FunctionReference on_start_fn;
  Napi::FunctionReference on_stop_fn;
  FloxMarketDataRecorderHandle handle{nullptr};

  struct TradePayload
  {
    FloxTradeData t;
  };
  struct BookPayload
  {
    uint32_t symbol;
    uint8_t is_snap;
    std::vector<FloxBookLevel> bids;
    std::vector<FloxBookLevel> asks;
    int64_t ts;
  };

  HookMode mode;
  Napi::Env env;

  MarketDataRecorderHookHost(Napi::Env env_, Napi::Object obj, HookMode m = HookMode::Sync)
      : TsfnHost(env_),
        on_trade_fn(takeFn(obj, "onTrade")),
        on_book_fn(takeFn(obj, "onBookUpdate")),
        on_start_fn(takeFn(obj, "onStart")),
        on_stop_fn(takeFn(obj, "onStop")),
        mode(m),
        env(env_)
  {
    if (mode == HookMode::Threaded)
    {
      openChannel("flox_recorder_cb");
    }
    FloxMarketDataRecorderCallbacks cb{};
    cb.on_trade = &MarketDataRecorderHookHost::onTradeBridge;
    cb.on_book_update = &MarketDataRecorderHookHost::onBookBridge;
    cb.on_start = &MarketDataRecorderHookHost::onStartBridge;
    cb.on_stop = &MarketDataRecorderHookHost::onStopBridge;
    cb.user_data = this;
    handle = flox_market_data_recorder_create(cb);
  }
  ~MarketDataRecorderHookHost()
  {
    if (handle)
    {
      flox_market_data_recorder_destroy(handle);
    }
  }
  MarketDataRecorderHookHost(const MarketDataRecorderHookHost&) = delete;
  MarketDataRecorderHookHost& operator=(const MarketDataRecorderHookHost&) = delete;

  static void onTradeBridge(void* ud, const FloxTradeData* t)
  {
    auto* self = static_cast<MarketDataRecorderHookHost*>(ud);
    if (self->on_trade_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->on_trade_fn.Call({tradeToJs(self->env, t)});
      return;
    }
    auto* p = new TradePayload{*t};
    self->post(p,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, TradePayload* tp)
               {
                 std::unique_ptr<TradePayload> guard(tp);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_trade_fn.Call({tradeToJs(env, &tp->t)});
               });
  }
  static void onBookBridge(void* ud, uint32_t symbol, uint8_t is_snap,
                           const FloxBookLevel* bids, uint32_t n_bids,
                           const FloxBookLevel* asks, uint32_t n_asks,
                           int64_t ts)
  {
    auto* self = static_cast<MarketDataRecorderHookHost*>(ud);
    if (self->on_book_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->on_book_fn.Call({
          Napi::Number::New(self->env, symbol),
          Napi::Boolean::New(self->env, is_snap != 0),
          bookLevelsToBigInt64(self->env, bids, n_bids),
          bookLevelsToBigInt64(self->env, asks, n_asks),
          Napi::BigInt::New(self->env, ts),
      });
      return;
    }
    auto* p = new BookPayload{symbol, is_snap, {}, {}, ts};
    p->bids.assign(bids, bids + n_bids);
    p->asks.assign(asks, asks + n_asks);
    self->post(p,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, BookPayload* bp)
               {
                 std::unique_ptr<BookPayload> guard(bp);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_book_fn.Call({
                     Napi::Number::New(env, bp->symbol),
                     Napi::Boolean::New(env, bp->is_snap != 0),
                     bookLevelsToBigInt64(env, bp->bids.data(),
                                          static_cast<uint32_t>(bp->bids.size())),
                     bookLevelsToBigInt64(env, bp->asks.data(),
                                          static_cast<uint32_t>(bp->asks.size())),
                     Napi::BigInt::New(env, bp->ts),
                 });
               });
  }
  static void onStartBridge(void* ud)
  {
    auto* self = static_cast<MarketDataRecorderHookHost*>(ud);
    if (self->on_start_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->on_start_fn.Call({});
      return;
    }
    self->post(new TsfnNoPayload{},
               [self, owner = self->owner()](Napi::Env, Napi::Function, TsfnNoPayload* np)
               {
                 std::unique_ptr<TsfnNoPayload> guard(np);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_start_fn.Call({});
               });
  }
  static void onStopBridge(void* ud)
  {
    auto* self = static_cast<MarketDataRecorderHookHost*>(ud);
    if (self->on_stop_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->on_stop_fn.Call({});
      return;
    }
    self->post(new TsfnNoPayload{},
               [self, owner = self->owner()](Napi::Env, Napi::Function, TsfnNoPayload* np)
               {
                 std::unique_ptr<TsfnNoPayload> guard(np);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_stop_fn.Call({});
               });
  }
};

// ── Executor ────────────────────────────────────────────────────────────

struct ExecutorHost : TsfnHost
{
  Napi::FunctionReference submit_fn;
  Napi::FunctionReference cancel_fn;
  Napi::FunctionReference cancel_all_fn;
  Napi::FunctionReference replace_fn;
  Napi::FunctionReference submit_oco_fn;
  Napi::FunctionReference capabilities_fn;
  Napi::FunctionReference on_start_fn;
  Napi::FunctionReference on_stop_fn;
  Napi::Env env;
  FloxExecutorHandle handle{nullptr};

  HookMode mode;

  ExecutorHost(Napi::Env env_, Napi::Object obj, HookMode m = HookMode::Sync)
      : TsfnHost(env_),
        submit_fn(takeFn(obj, "submit")),
        cancel_fn(takeFn(obj, "cancel")),
        cancel_all_fn(takeFn(obj, "cancelAll")),
        replace_fn(takeFn(obj, "replace")),
        submit_oco_fn(takeFn(obj, "submitOco")),
        capabilities_fn(takeFn(obj, "capabilities")),
        on_start_fn(takeFn(obj, "onStart")),
        on_stop_fn(takeFn(obj, "onStop")),
        env(env_),
        mode(m)
  {
    if (mode == HookMode::Threaded)
    {
      openChannel("flox_executor_cb");
    }
    FloxExecutorCallbacks cb{};
    cb.submit = &ExecutorHost::submitBridge;
    cb.cancel = &ExecutorHost::cancelBridge;
    cb.cancel_all = &ExecutorHost::cancelAllBridge;
    cb.replace = &ExecutorHost::replaceBridge;
    cb.submit_oco = &ExecutorHost::submitOcoBridge;
    cb.capabilities = &ExecutorHost::capabilitiesBridge;
    cb.on_start = &ExecutorHost::onStartBridge;
    cb.on_stop = &ExecutorHost::onStopBridge;
    cb.user_data = this;
    handle = flox_executor_create(cb);
  }
  ~ExecutorHost()
  {
    if (handle)
    {
      flox_executor_destroy(handle);
    }
  }
  ExecutorHost(const ExecutorHost&) = delete;
  ExecutorHost& operator=(const ExecutorHost&) = delete;

  struct OrderPayload
  {
    FloxOrder o;
  };
  struct ReplacePayload
  {
    uint64_t old_id;
    FloxOrder o;
  };
  struct OcoPayload
  {
    FloxOrder a;
    FloxOrder b;
  };

  static void submitBridge(void* ud, const FloxOrder* o)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->submit_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->submit_fn.Call({orderToJs(self->env, o)});
      return;
    }
    auto* p = new OrderPayload{*o};
    self->post(p,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, OrderPayload* op)
               {
                 std::unique_ptr<OrderPayload> guard(op);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->submit_fn.Call({orderToJs(env, &op->o)});
               });
  }
  static void cancelBridge(void* ud, uint64_t id)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->cancel_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->cancel_fn.Call({Napi::Number::New(self->env, static_cast<double>(id))});
      return;
    }
    auto* p = new uint64_t(id);
    self->post(p,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, uint64_t* idp)
               {
                 std::unique_ptr<uint64_t> guard(idp);
                 self->cancel_fn.Call(
                     {Napi::Number::New(env, static_cast<double>(*idp))});
               });
  }
  static void cancelAllBridge(void* ud, uint32_t s)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->cancel_all_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->cancel_all_fn.Call({Napi::Number::New(self->env, s)});
      return;
    }
    auto* p = new uint32_t(s);
    self->post(p,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, uint32_t* sp)
               {
                 std::unique_ptr<uint32_t> guard(sp);
                 self->cancel_all_fn.Call({Napi::Number::New(env, *sp)});
               });
  }
  static void replaceBridge(void* ud, uint64_t old_id, const FloxOrder* o)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->replace_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->replace_fn.Call({
          Napi::Number::New(self->env, static_cast<double>(old_id)),
          orderToJs(self->env, o),
      });
      return;
    }
    auto* p = new ReplacePayload{old_id, *o};
    self->post(p,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, ReplacePayload* rp)
               {
                 std::unique_ptr<ReplacePayload> guard(rp);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->replace_fn.Call({
                     Napi::Number::New(env, static_cast<double>(rp->old_id)),
                     orderToJs(env, &rp->o),
                 });
               });
  }
  static void submitOcoBridge(void* ud, const FloxOrder* a, const FloxOrder* b)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->submit_oco_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->submit_oco_fn.Call({orderToJs(self->env, a), orderToJs(self->env, b)});
      return;
    }
    auto* p = new OcoPayload{*a, *b};
    self->post(p,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, OcoPayload* op)
               {
                 std::unique_ptr<OcoPayload> guard(op);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->submit_oco_fn.Call({
                     orderToJs(env, &op->a),
                     orderToJs(env, &op->b),
                 });
               });
  }
  // Capabilities is synchronous — engine queries it inline. Only safe
  // to call from the JS thread (sync Runner / BacktestRunner).
  static void capabilitiesBridge(void* ud, FloxExchangeCapabilities* out)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    *out = FloxExchangeCapabilities{};
    if (self->capabilities_fn.IsEmpty())
    {
      return;
    }
    auto result = self->capabilities_fn.Call({});
    if (!result.IsObject())
    {
      return;
    }
    auto obj = result.As<Napi::Object>();
    auto getBool = [&](const char* k) -> uint8_t
    {
      auto v = obj.Get(k);
      return (v.IsBoolean() && v.As<Napi::Boolean>().Value()) ? 1u : 0u;
    };
    out->supports_stop_market = getBool("stopMarket");
    out->supports_stop_limit = getBool("stopLimit");
    out->supports_take_profit_market = getBool("takeProfitMarket");
    out->supports_take_profit_limit = getBool("takeProfitLimit");
    out->supports_trailing_stop = getBool("trailingStop");
    out->supports_iceberg = getBool("iceberg");
    out->supports_oco = getBool("oco");
    out->supports_gtc = getBool("gtc");
    out->supports_ioc = getBool("ioc");
    out->supports_fok = getBool("fok");
    out->supports_gtd = getBool("gtd");
    out->supports_post_only = getBool("postOnly");
    out->supports_reduce_only = getBool("reduceOnly");
    out->supports_close_position = getBool("closePosition");
  }
  static void onStartBridge(void* ud)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->on_start_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->on_start_fn.Call({});
      return;
    }
    self->post(new TsfnNoPayload{},
               [self, owner = self->owner()](Napi::Env, Napi::Function, TsfnNoPayload* np)
               {
                 std::unique_ptr<TsfnNoPayload> guard(np);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_start_fn.Call({});
               });
  }
  static void onStopBridge(void* ud)
  {
    auto* self = static_cast<ExecutorHost*>(ud);
    if (self->on_stop_fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      self->on_stop_fn.Call({});
      return;
    }
    self->post(new TsfnNoPayload{},
               [self, owner = self->owner()](Napi::Env, Napi::Function, TsfnNoPayload* np)
               {
                 std::unique_ptr<TsfnNoPayload> guard(np);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_stop_fn.Call({});
               });
  }
};

// ── ExecutionListener ───────────────────────────────────────────────────

struct ExecutionListenerHost : TsfnHost
{
  Napi::FunctionReference on_submitted_fn;
  Napi::FunctionReference on_accepted_fn;
  Napi::FunctionReference on_partial_fn;
  Napi::FunctionReference on_filled_fn;
  Napi::FunctionReference on_pending_cancel_fn;
  Napi::FunctionReference on_canceled_fn;
  Napi::FunctionReference on_expired_fn;
  Napi::FunctionReference on_rejected_fn;
  Napi::FunctionReference on_replaced_fn;
  Napi::FunctionReference on_pending_trigger_fn;
  Napi::FunctionReference on_triggered_fn;
  Napi::FunctionReference on_trailing_update_fn;
  FloxExecutionListenerHandle handle{nullptr};

  HookMode mode;
  Napi::Env env;

  ExecutionListenerHost(Napi::Env env_, Napi::Object obj, HookMode m = HookMode::Sync)
      : TsfnHost(env_),
        on_submitted_fn(takeFn(obj, "onSubmitted")),
        on_accepted_fn(takeFn(obj, "onAccepted")),
        on_partial_fn(takeFn(obj, "onPartiallyFilled")),
        on_filled_fn(takeFn(obj, "onFilled")),
        on_pending_cancel_fn(takeFn(obj, "onPendingCancel")),
        on_canceled_fn(takeFn(obj, "onCanceled")),
        on_expired_fn(takeFn(obj, "onExpired")),
        on_rejected_fn(takeFn(obj, "onRejected")),
        on_replaced_fn(takeFn(obj, "onReplaced")),
        on_pending_trigger_fn(takeFn(obj, "onPendingTrigger")),
        on_triggered_fn(takeFn(obj, "onTriggered")),
        on_trailing_update_fn(takeFn(obj, "onTrailingStopUpdated")),
        mode(m),
        env(env_)
  {
    if (mode == HookMode::Threaded)
    {
      openChannel("flox_listener_cb");
    }
    FloxExecutionListenerCallbacks cb{};
    cb.on_submitted = &ExecutionListenerHost::onSubmittedBridge;
    cb.on_accepted = &ExecutionListenerHost::onAcceptedBridge;
    cb.on_partially_filled = &ExecutionListenerHost::onPartialBridge;
    cb.on_filled = &ExecutionListenerHost::onFilledBridge;
    cb.on_pending_cancel = &ExecutionListenerHost::onPendingCancelBridge;
    cb.on_canceled = &ExecutionListenerHost::onCanceledBridge;
    cb.on_expired = &ExecutionListenerHost::onExpiredBridge;
    cb.on_rejected = &ExecutionListenerHost::onRejectedBridge;
    cb.on_replaced = &ExecutionListenerHost::onReplacedBridge;
    cb.on_pending_trigger = &ExecutionListenerHost::onPendingTriggerBridge;
    cb.on_triggered = &ExecutionListenerHost::onTriggeredBridge;
    cb.on_trailing_stop_updated = &ExecutionListenerHost::onTrailingUpdateBridge;
    cb.user_data = this;
    handle = flox_execution_listener_create(cb);
  }
  ~ExecutionListenerHost()
  {
    if (handle)
    {
      flox_execution_listener_destroy(handle);
    }
  }
  ExecutionListenerHost(const ExecutionListenerHost&) = delete;
  ExecutionListenerHost& operator=(const ExecutionListenerHost&) = delete;

  struct OrderEv
  {
    FloxOrder o;
  };
  struct PartialEv
  {
    FloxOrder o;
    int64_t fill_qty;
  };
  struct RejectEv
  {
    FloxOrder o;
    std::string reason;
  };
  struct ReplaceEv
  {
    FloxOrder a;
    FloxOrder b;
  };
  struct TrailingEv
  {
    FloxOrder o;
    int64_t new_trigger;
  };

  static void singleOrderBridge(ExecutionListenerHost* self, Napi::FunctionReference& fn,
                                const FloxOrder* o)
  {
    if (fn.IsEmpty())
    {
      return;
    }
    if (self->mode == HookMode::Sync)
    {
      fn.Call({orderToJs(self->env, o)});
      return;
    }
    auto* ev = new OrderEv{*o};
    self->post(ev,
               [&fn, owner = self->owner()](Napi::Env env, Napi::Function, OrderEv* e)
               {
                 std::unique_ptr<OrderEv> guard(e);
                 if (owner->retired())
                 {
                   return;
                 }
                 fn.Call({orderToJs(env, &e->o)});
               });
  }

  static void onSubmittedBridge(void* ud, const FloxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_submitted_fn, o);
  }
  static void onAcceptedBridge(void* ud, const FloxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_accepted_fn, o);
  }
  static void onFilledBridge(void* ud, const FloxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_filled_fn, o);
  }
  static void onPendingCancelBridge(void* ud, const FloxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_pending_cancel_fn, o);
  }
  static void onCanceledBridge(void* ud, const FloxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_canceled_fn, o);
  }
  static void onExpiredBridge(void* ud, const FloxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_expired_fn, o);
  }
  static void onPendingTriggerBridge(void* ud, const FloxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_pending_trigger_fn, o);
  }
  static void onTriggeredBridge(void* ud, const FloxOrder* o)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    singleOrderBridge(self, self->on_triggered_fn, o);
  }
  static void onPartialBridge(void* ud, const FloxOrder* o, int64_t fill_qty)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    if (self->on_partial_fn.IsEmpty())
    {
      return;
    }
    auto* ev = new PartialEv{*o, fill_qty};
    self->post(ev,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, PartialEv* e)
               {
                 std::unique_ptr<PartialEv> guard(e);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_partial_fn.Call({
                     orderToJs(env, &e->o),
                     Napi::Number::New(env, e->fill_qty / 1e8),
                 });
               });
  }
  static void onRejectedBridge(void* ud, const FloxOrder* o, const char* reason)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    if (self->on_rejected_fn.IsEmpty())
    {
      return;
    }
    auto* ev = new RejectEv{*o, reason ? std::string(reason) : std::string{}};
    self->post(ev,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, RejectEv* e)
               {
                 std::unique_ptr<RejectEv> guard(e);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_rejected_fn.Call({
                     orderToJs(env, &e->o),
                     Napi::String::New(env, e->reason),
                 });
               });
  }
  static void onReplacedBridge(void* ud, const FloxOrder* a, const FloxOrder* b)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    if (self->on_replaced_fn.IsEmpty())
    {
      return;
    }
    auto* ev = new ReplaceEv{*a, *b};
    self->post(ev,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, ReplaceEv* e)
               {
                 std::unique_ptr<ReplaceEv> guard(e);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_replaced_fn.Call({
                     orderToJs(env, &e->a),
                     orderToJs(env, &e->b),
                 });
               });
  }
  static void onTrailingUpdateBridge(void* ud, const FloxOrder* o, int64_t new_trigger_raw)
  {
    auto* self = static_cast<ExecutionListenerHost*>(ud);
    if (self->on_trailing_update_fn.IsEmpty())
    {
      return;
    }
    auto* ev = new TrailingEv{*o, new_trigger_raw};
    self->post(ev,
               [self, owner = self->owner()](Napi::Env env, Napi::Function, TrailingEv* e)
               {
                 std::unique_ptr<TrailingEv> guard(e);
                 if (owner->retired())
                 {
                   return;
                 }
                 self->on_trailing_update_fn.Call({
                     orderToJs(env, &e->o),
                     Napi::Number::New(env, e->new_trigger / 1e8),
                 });
               });
  }
};

// ── Logger callback ─────────────────────────────────────────────────────

// The one owner whose channel is not tied to a runner: the C ABI logger is
// process-scoped, so install/uninstall are its open/close, and a JS log
// callback deliberately keeps the event loop alive while it is installed.
// The instance is a function-local static, so `self` in the queued callback
// below can never dangle and needs no liveness flag.
struct LoggerCallback
{
  Napi::FunctionReference fn;
  std::unique_ptr<TsfnChannel> chan;
  bool active{false};

  void install(Napi::Env env, Napi::Function f)
  {
    if (active)
    {
      uninstall();
    }
    fn = Napi::Persistent(f);
    auto noop = Napi::Function::New(env, [](const Napi::CallbackInfo&) {});
    chan = std::make_unique<TsfnChannel>(env, noop, "flox_logger_cb");
    flox_set_log_callback(&LoggerCallback::bridge, this);
    active = true;
  }
  void uninstall()
  {
    if (!active)
    {
      return;
    }
    flox_set_log_callback(nullptr, nullptr);
    chan->close();
    chan.reset();
    fn.Reset();
    active = false;
  }
  static void bridge(void* ud, int32_t level, const char* msg)
  {
    auto* self = static_cast<LoggerCallback*>(ud);
    if (self->fn.IsEmpty())
    {
      return;
    }
    struct LogPayload
    {
      int32_t level;
      std::string msg;
    };
    auto* p = new LogPayload{level, msg ? std::string(msg) : std::string{}};
    if (!self->chan)
    {
      delete p;
      return;
    }
    self->chan->call(p,
                     [self](Napi::Env env, Napi::Function, LogPayload* lp)
                     {
                       std::unique_ptr<LogPayload> guard(lp);
                       self->fn.Call({
                           Napi::Number::New(env, lp->level),
                           Napi::String::New(env, lp->msg),
                       });
                     });
  }
};

// Global, single instance. Logger is process-scoped in the C ABI.
inline LoggerCallback& globalLogger()
{
  static LoggerCallback inst;
  return inst;
}

inline Napi::Value setLogCallback(const Napi::CallbackInfo& info)
{
  auto env = info.Env();
  if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
  {
    globalLogger().uninstall();
    return env.Undefined();
  }
  if (!info[0].IsFunction())
  {
    Napi::TypeError::New(env, "expected function or null").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  globalLogger().install(env, info[0].As<Napi::Function>());
  return env.Undefined();
}

// Register setLogCallback on the module exports object. The d.ts gate
// (scripts/check_dts_exports.py) scans *.h files for exports.Set(...)
// patterns, so the registration call lives here rather than in the .cpp.
inline void registerHooks(Napi::Env env, Napi::Object exports)
{
  exports.Set("setLogCallback", Napi::Function::New(env, &setLogCallback));
}

}  // namespace flox_node
