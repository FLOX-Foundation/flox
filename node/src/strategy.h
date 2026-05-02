// node/src/strategy.h — StrategyRunner and LiveEngine for Node.js

#pragma once

#include <napi.h>

#include "flox/capi/bridge_strategy.h"
#include "flox/capi/flox_capi.h"
#include "flox/engine/symbol_registry.h"

#include <memory>
#include <string>
#include <vector>

namespace node_flox
{

using namespace flox;

// ──────────────────────────────────────────────────────────────
// SymbolRegistryNode — owns a SymbolRegistry, exposes addSymbol
// ──────────────────────────────────────────────────────────────

class SymbolRegistryNode : public Napi::ObjectWrap<SymbolRegistryNode>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "SymbolRegistry",
                       {
                           InstanceMethod("addSymbol", &SymbolRegistryNode::addSymbol),
                           InstanceMethod("symbolCount", &SymbolRegistryNode::symbolCount),
                       });
  }

  SymbolRegistryNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<SymbolRegistryNode>(info)
  {
    _reg = std::make_unique<SymbolRegistry>();
  }

  SymbolRegistry* get() { return _reg.get(); }

 private:
  std::unique_ptr<SymbolRegistry> _reg;

  Napi::Value addSymbol(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    std::string ex = info[0].As<Napi::String>().Utf8Value();
    std::string sym = info[1].As<Napi::String>().Utf8Value();
    double tickSize = info.Length() > 2 ? info[2].As<Napi::Number>().DoubleValue() : 0.01;
    flox::SymbolInfo si;
    si.exchange = ex;
    si.symbol = sym;
    si.tickSize = flox::Price::fromDouble(tickSize);
    uint32_t id = static_cast<uint32_t>(_reg->registerSymbol(si));

    // Return a Symbol object: { id, exchange, name, tickSize, valueOf() → id }
    // valueOf() makes it transparently usable as a number everywhere.
    auto obj = Napi::Object::New(env);
    obj.Set("id", Napi::Number::New(env, id));
    obj.Set("exchange", Napi::String::New(env, ex));
    obj.Set("name", Napi::String::New(env, sym));
    obj.Set("tickSize", Napi::Number::New(env, tickSize));
    obj.Set("valueOf", Napi::Function::New(env,
                                           [](const Napi::CallbackInfo& i) -> Napi::Value
                                           {
                                             return i.This().As<Napi::Object>().Get("id");
                                           }));
    obj.Set("toString", Napi::Function::New(env,
                                            [ex, sym, id](const Napi::CallbackInfo& i) -> Napi::Value
                                            {
                                              return Napi::String::New(i.Env(),
                                                                       "Symbol(" + ex + ":" + sym + ", id=" + std::to_string(id) + ")");
                                            }));
    return obj;
  }

  Napi::Value symbolCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_reg->size()));
  }
};

// ──────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────

// Accept Symbol object or plain number → uint32_t id.
inline uint32_t symId(const Napi::Value& v)
{
  if (v.IsObject())
  {
    return v.As<Napi::Object>().Get("id").As<Napi::Number>().Uint32Value();
  }
  return v.As<Napi::Number>().Uint32Value();
}

static constexpr const char* kOrderTypeNames[] = {
    "market", "limit", "stop_market", "stop_limit",
    "tp_market", "tp_limit", "trailing_stop",
    "cancel", "cancel_all", "modify"};

inline Napi::Object signalToJs(Napi::Env env, const FloxSignal* s)
{
  auto obj = Napi::Object::New(env);
  obj.Set("orderId", Napi::Number::New(env, static_cast<double>(s->order_id)));
  obj.Set("symbol", Napi::Number::New(env, s->symbol));
  obj.Set("side", Napi::String::New(env, s->side == 0 ? "buy" : "sell"));
  obj.Set("orderType", Napi::String::New(env,
                                         s->order_type < 10 ? kOrderTypeNames[s->order_type] : "unknown"));
  obj.Set("price", Napi::Number::New(env, s->price));
  obj.Set("quantity", Napi::Number::New(env, s->quantity));
  obj.Set("triggerPrice", Napi::Number::New(env, s->trigger_price));
  obj.Set("trailingOffset", Napi::Number::New(env, s->trailing_offset));
  obj.Set("trailingBps", Napi::Number::New(env, s->trailing_bps));
  obj.Set("newPrice", Napi::Number::New(env, s->new_price));
  obj.Set("newQuantity", Napi::Number::New(env, s->new_quantity));
  return obj;
}

// ──────────────────────────────────────────────────────────────
// NodeStrategy — wraps a JS object with on_trade / on_book_update
// callbacks, owns a BridgeStrategy.
// ──────────────────────────────────────────────────────────────

struct NodeStrategyHost
{
  Napi::Env env;
  Napi::FunctionReference on_trade_fn;
  Napi::FunctionReference on_book_fn;
  Napi::FunctionReference on_bar_fn;
  Napi::FunctionReference on_start_fn;
  Napi::FunctionReference on_stop_fn;
  Napi::ObjectReference emitter;
  std::vector<uint32_t> syms;
  std::unique_ptr<BridgeStrategy> bridge;

  // Set by enableThreaded() when host is used with LiveEngine.
  // onTrade / onBook / onStart / onStop queue via TSFN instead of
  // calling V8 directly (which is illegal from a C++ consumer thread).
  bool _threaded{false};
  Napi::ThreadSafeFunction _tsfn;

  struct TradeCallData
  {
    FloxSymbolContext ctx;
    FloxTradeData trade;
    NodeStrategyHost* host;
  };
  struct BookCallData
  {
    FloxSymbolContext ctx;
    NodeStrategyHost* host;
  };
  struct BarCallData
  {
    FloxSymbolContext ctx;
    FloxBarData bar;
    NodeStrategyHost* host;
  };

  void enableThreaded()
  {
    _threaded = true;
    auto noop = Napi::Function::New(env, [](const Napi::CallbackInfo&) {});
    _tsfn = Napi::ThreadSafeFunction::New(env, noop, "flox_strategy_cb", 0, 1);
  }

  ~NodeStrategyHost()
  {
    if (_threaded)
    {
      _tsfn.Release();
    }
  }

  NodeStrategyHost(Napi::Env env_, Napi::Object strategy_obj,
                   SymbolRegistry* reg, uint32_t id,
                   const std::vector<uint32_t>& syms_)
      : env(env_), syms(syms_)
  {
    auto get = [&](const char* name) -> Napi::FunctionReference
    {
      auto val = strategy_obj.Get(name);
      if (val.IsFunction())
      {
        return Napi::Persistent(val.As<Napi::Function>());
      }
      return {};
    };
    on_trade_fn = get("onTrade");
    on_book_fn = get("onBookUpdate");
    on_bar_fn = get("onBar");
    on_start_fn = get("onStart");
    on_stop_fn = get("onStop");

    FloxStrategyCallbacks cbs{};
    cbs.user_data = this;
    cbs.on_trade = &NodeStrategyHost::onTrade;
    cbs.on_book = &NodeStrategyHost::onBook;
    cbs.on_bar = &NodeStrategyHost::onBar;
    cbs.on_start = &NodeStrategyHost::onStart;
    cbs.on_stop = &NodeStrategyHost::onStop;

    bridge = std::make_unique<BridgeStrategy>(
        static_cast<SubscriberId>(id), syms, *reg, cbs);

    // Build emitter: order emission functions bound to this bridge.
    // Created once per host; passed to every onTrade / onBook callback.
    auto em = Napi::Object::New(env_);
    uint32_t defaultSym = syms.empty() ? 0 : syms[0];

    em.Set("marketBuy", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                            {
        auto* h   = static_cast<NodeStrategyHost*>(i.Data());
        double qty = i[0].As<Napi::Number>().DoubleValue();
        uint32_t sym = i.Length() > 1 ? symId(i[1])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitMarketBuy(sym, Quantity::fromDouble(qty));
        return i.Env().Undefined(); }, "marketBuy", this));

    em.Set("marketSell", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                             {
        auto* h   = static_cast<NodeStrategyHost*>(i.Data());
        double qty = i[0].As<Napi::Number>().DoubleValue();
        uint32_t sym = i.Length() > 1 ? symId(i[1])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitMarketSell(sym, Quantity::fromDouble(qty));
        return i.Env().Undefined(); }, "marketSell", this));

    em.Set("limitBuy", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                           {
        auto* h    = static_cast<NodeStrategyHost*>(i.Data());
        double price = i[0].As<Napi::Number>().DoubleValue();
        double qty   = i[1].As<Napi::Number>().DoubleValue();
        uint32_t sym = i.Length() > 2 ? symId(i[2])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitLimitBuy(sym, Price::fromDouble(price), Quantity::fromDouble(qty));
        return i.Env().Undefined(); }, "limitBuy", this));

    em.Set("limitSell", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                            {
        auto* h    = static_cast<NodeStrategyHost*>(i.Data());
        double price = i[0].As<Napi::Number>().DoubleValue();
        double qty   = i[1].As<Napi::Number>().DoubleValue();
        uint32_t sym = i.Length() > 2 ? symId(i[2])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitLimitSell(sym, Price::fromDouble(price), Quantity::fromDouble(qty));
        return i.Env().Undefined(); }, "limitSell", this));

    em.Set("cancel", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                         {
        auto* h = static_cast<NodeStrategyHost*>(i.Data());
        uint64_t orderId = static_cast<uint64_t>(i[0].As<Napi::Number>().Int64Value());
        h->bridge->publicEmitCancel(orderId);
        return i.Env().Undefined(); }, "cancel", this));

    em.Set("closePosition", Napi::Function::New(env_, [](const Napi::CallbackInfo& i) -> Napi::Value
                                                {
        auto* h  = static_cast<NodeStrategyHost*>(i.Data());
        uint32_t sym = i.Length() > 0 ? symId(i[0])
                                       : (h->syms.empty() ? 0u : h->syms[0]);
        h->bridge->publicEmitClosePosition(sym);
        return i.Env().Undefined(); }, "closePosition", this));

    (void)defaultSym;
    emitter = Napi::Persistent(em);
    emitter.SuppressDestruct();
  }

  static void buildCtxObj(Napi::Env env, Napi::Object& o, const FloxSymbolContext* ctx)
  {
    o.Set("symbolId", Napi::Number::New(env, ctx->symbol_id));
    o.Set("position", Napi::Number::New(env, flox_quantity_to_double(ctx->position_raw)));
    o.Set("lastTradePrice", Napi::Number::New(env, flox_price_to_double(ctx->last_trade_price_raw)));
    o.Set("bestBid", Napi::Number::New(env, flox_price_to_double(ctx->book.bid_price_raw)));
    o.Set("bestAsk", Napi::Number::New(env, flox_price_to_double(ctx->book.ask_price_raw)));
    o.Set("midPrice", Napi::Number::New(env, flox_price_to_double(ctx->book.mid_raw)));
  }

  static void callOnTrade(Napi::Env env, Napi::Function, TradeCallData* d)
  {
    auto* self = d->host;
    if (!self->on_trade_fn.IsEmpty())
    {
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, &d->ctx);
      auto tradeObj = Napi::Object::New(env);
      tradeObj.Set("symbol", Napi::Number::New(env, d->trade.symbol));
      tradeObj.Set("price", Napi::Number::New(env, flox_price_to_double(d->trade.price_raw)));
      tradeObj.Set("qty", Napi::Number::New(env, flox_quantity_to_double(d->trade.quantity_raw)));
      tradeObj.Set("isBuy", Napi::Boolean::New(env, d->trade.is_buy != 0));
      tradeObj.Set("side", Napi::String::New(env, d->trade.is_buy ? "buy" : "sell"));
      tradeObj.Set("timestampNs", Napi::Number::New(env, static_cast<double>(d->trade.exchange_ts_ns)));
      self->on_trade_fn.Call({ctxObj, tradeObj, self->emitter.Value()});
    }
    delete d;
  }

  static void callOnBook(Napi::Env env, Napi::Function, BookCallData* d)
  {
    auto* self = d->host;
    if (!self->on_book_fn.IsEmpty())
    {
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, &d->ctx);
      self->on_book_fn.Call({ctxObj, self->emitter.Value()});
    }
    delete d;
  }

  static void buildBarObj(Napi::Env env, Napi::Object& o, const FloxBarData* bar)
  {
    o.Set("symbol", Napi::Number::New(env, bar->symbol));
    o.Set("barType", Napi::Number::New(env, bar->bar_type));
    o.Set("barTypeParam", Napi::Number::New(env, static_cast<double>(bar->bar_type_param)));
    o.Set("open", Napi::Number::New(env, flox_price_to_double(bar->open_raw)));
    o.Set("high", Napi::Number::New(env, flox_price_to_double(bar->high_raw)));
    o.Set("low", Napi::Number::New(env, flox_price_to_double(bar->low_raw)));
    o.Set("close", Napi::Number::New(env, flox_price_to_double(bar->close_raw)));
    o.Set("volume", Napi::Number::New(env, flox_quantity_to_double(bar->volume_raw)));
    o.Set("buyVolume", Napi::Number::New(env, flox_quantity_to_double(bar->buy_volume_raw)));
    o.Set("startTimeNs", Napi::Number::New(env, static_cast<double>(bar->start_time_ns)));
    o.Set("endTimeNs", Napi::Number::New(env, static_cast<double>(bar->end_time_ns)));
    o.Set("closeReason", Napi::Number::New(env, bar->close_reason));
  }

  static void callOnBar(Napi::Env env, Napi::Function, BarCallData* d)
  {
    auto* self = d->host;
    if (!self->on_bar_fn.IsEmpty())
    {
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, &d->ctx);
      auto barObj = Napi::Object::New(env);
      buildBarObj(env, barObj, &d->bar);
      self->on_bar_fn.Call({ctxObj, barObj, self->emitter.Value()});
    }
    delete d;
  }

  static void onTrade(void* ud, const FloxSymbolContext* ctx,
                      const FloxTradeData* trade)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (self->on_trade_fn.IsEmpty())
    {
      return;
    }

    if (self->_threaded)
    {
      auto* d = new TradeCallData{*ctx, *trade, self};
      self->_tsfn.NonBlockingCall(d, &NodeStrategyHost::callOnTrade);
    }
    else
    {
      auto env = self->env;
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, ctx);
      auto tradeObj = Napi::Object::New(env);
      tradeObj.Set("symbol", Napi::Number::New(env, trade->symbol));
      tradeObj.Set("price", Napi::Number::New(env, flox_price_to_double(trade->price_raw)));
      tradeObj.Set("qty", Napi::Number::New(env, flox_quantity_to_double(trade->quantity_raw)));
      tradeObj.Set("isBuy", Napi::Boolean::New(env, trade->is_buy != 0));
      tradeObj.Set("side", Napi::String::New(env, trade->is_buy ? "buy" : "sell"));
      tradeObj.Set("timestampNs", Napi::Number::New(env, static_cast<double>(trade->exchange_ts_ns)));
      self->on_trade_fn.Call({ctxObj, tradeObj, self->emitter.Value()});
    }
  }

  static void onBook(void* ud, const FloxSymbolContext* ctx,
                     const FloxBookData* /*book*/)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (self->on_book_fn.IsEmpty())
    {
      return;
    }

    if (self->_threaded)
    {
      auto* d = new BookCallData{*ctx, self};
      self->_tsfn.NonBlockingCall(d, &NodeStrategyHost::callOnBook);
    }
    else
    {
      auto env = self->env;
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, ctx);
      self->on_book_fn.Call({ctxObj, self->emitter.Value()});
    }
  }

  static void onBar(void* ud, const FloxSymbolContext* ctx,
                    const FloxBarData* bar)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (self->on_bar_fn.IsEmpty())
    {
      return;
    }

    if (self->_threaded)
    {
      auto* d = new BarCallData{*ctx, *bar, self};
      self->_tsfn.NonBlockingCall(d, &NodeStrategyHost::callOnBar);
    }
    else
    {
      auto env = self->env;
      auto ctxObj = Napi::Object::New(env);
      buildCtxObj(env, ctxObj, ctx);
      auto barObj = Napi::Object::New(env);
      buildBarObj(env, barObj, bar);
      self->on_bar_fn.Call({ctxObj, barObj, self->emitter.Value()});
    }
  }

  struct LifecycleCallData
  {
    NodeStrategyHost* host;
    bool is_start;
  };

  static void onStart(void* ud)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (self->on_start_fn.IsEmpty())
    {
      return;
    }
    if (self->_threaded)
    {
      auto* d = new LifecycleCallData{self, true};
      self->_tsfn.NonBlockingCall(d, [](Napi::Env, Napi::Function, LifecycleCallData* d)
                                  {
        d->host->on_start_fn.Call({});
        delete d; });
    }
    else
    {
      self->on_start_fn.Call({});
    }
  }

  static void onStop(void* ud)
  {
    auto* self = static_cast<NodeStrategyHost*>(ud);
    if (self->on_stop_fn.IsEmpty())
    {
      return;
    }
    if (self->_threaded)
    {
      auto* d = new LifecycleCallData{self, false};
      self->_tsfn.NonBlockingCall(d, [](Napi::Env, Napi::Function, LifecycleCallData* d)
                                  {
        d->host->on_stop_fn.Call({});
        delete d; });
    }
    else
    {
      self->on_stop_fn.Call({});
    }
  }
};

// ──────────────────────────────────────────────────────────────
// StrategyRunnerNode — synchronous runner (JS thread)
// ──────────────────────────────────────────────────────────────

class StrategyRunnerNode : public Napi::ObjectWrap<StrategyRunnerNode>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "StrategyRunner",
                       {
                           InstanceMethod("addStrategy", &StrategyRunnerNode::addStrategy),
                           InstanceMethod("start", &StrategyRunnerNode::start),
                           InstanceMethod("stop", &StrategyRunnerNode::stop),
                           InstanceMethod("onTrade", &StrategyRunnerNode::onTrade),
                           InstanceMethod("onBookSnapshot", &StrategyRunnerNode::onBookSnapshot),
                           InstanceMethod("onBar", &StrategyRunnerNode::onBar),
                       });
  }

  StrategyRunnerNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<StrategyRunnerNode>(info)
  {
    auto env = info.Env();
    if (info.Length() < 2)
    {
      Napi::TypeError::New(env, "StrategyRunner(registry, onSignal)").ThrowAsJavaScriptException();
    }

    _reg = Napi::ObjectWrap<SymbolRegistryNode>::Unwrap(info[0].As<Napi::Object>())->get();
    _on_signal = Napi::Persistent(info[1].As<Napi::Function>());
    _runner = flox_runner_create(static_cast<FloxRegistryHandle>(_reg),
                                 &StrategyRunnerNode::signalCb, this);
  }

  ~StrategyRunnerNode()
  {
    if (_runner)
    {
      flox_runner_destroy(_runner);
    }
  }

 private:
  SymbolRegistry* _reg{nullptr};
  FloxRunnerHandle _runner{nullptr};
  Napi::FunctionReference _on_signal;
  std::vector<std::unique_ptr<NodeStrategyHost>> _hosts;

  Napi::Value addStrategy(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    auto obj = info[0].As<Napi::Object>();
    auto symsV = obj.Get("symbols");
    std::vector<uint32_t> syms;
    if (symsV.IsArray())
    {
      auto arr = symsV.As<Napi::Array>();
      for (uint32_t i = 0; i < arr.Length(); ++i)
      {
        syms.push_back(symId(arr.Get(i)));
      }
    }
    uint32_t id = static_cast<uint32_t>(_hosts.size()) + 1;
    auto host = std::make_unique<NodeStrategyHost>(env, obj, _reg, id, syms);
    flox_runner_add_strategy(_runner,
                             static_cast<FloxStrategyHandle>(host->bridge.get()));
    _hosts.push_back(std::move(host));
    return env.Undefined();
  }

  Napi::Value start(const Napi::CallbackInfo& info)
  {
    flox_runner_start(_runner);
    return info.Env().Undefined();
  }

  Napi::Value stop(const Napi::CallbackInfo& info)
  {
    flox_runner_stop(_runner);
    return info.Env().Undefined();
  }

  Napi::Value onTrade(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    double price = info[1].As<Napi::Number>().DoubleValue();
    double qty = info[2].As<Napi::Number>().DoubleValue();
    bool isBuy = info[3].As<Napi::Boolean>().Value();
    int64_t ts = info.Length() > 4 ? static_cast<int64_t>(info[4].As<Napi::Number>().Int64Value()) : 0;
    flox_runner_on_trade(_runner, sym, price, qty, isBuy ? 1 : 0, ts);
    return info.Env().Undefined();
  }

  Napi::Value onBookSnapshot(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint32_t sym = symId(info[0]);
    auto bidP = info[1].As<Napi::Array>();
    auto bidQ = info[2].As<Napi::Array>();
    auto askP = info[3].As<Napi::Array>();
    auto askQ = info[4].As<Napi::Array>();
    int64_t ts = info.Length() > 5 ? static_cast<int64_t>(info[5].As<Napi::Number>().Int64Value()) : 0;

    auto toVec = [](const Napi::Array& a)
    {
      std::vector<double> v(a.Length());
      for (uint32_t i = 0; i < a.Length(); ++i)
      {
        v[i] = a.Get(i).As<Napi::Number>().DoubleValue();
      }
      return v;
    };
    auto bp = toVec(bidP), bq = toVec(bidQ), ap = toVec(askP), aq = toVec(askQ);
    flox_runner_on_book_snapshot(_runner, sym,
                                 bp.data(), bq.data(), static_cast<uint32_t>(bp.size()),
                                 ap.data(), aq.data(), static_cast<uint32_t>(ap.size()), ts);
    return env.Undefined();
  }

  // onBar(symbol, { open, high, low, close, volume?, buyVolume?,
  //                 startTimeNs?, endTimeNs?, barType?, barTypeParam?,
  //                 closeReason? })
  Napi::Value onBar(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint32_t sym = symId(info[0]);
    auto opts = info[1].As<Napi::Object>();
    auto getNum = [&](const char* k, double dflt) -> double
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? v.As<Napi::Number>().DoubleValue() : dflt;
    };
    auto getInt = [&](const char* k, int64_t dflt) -> int64_t
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? static_cast<int64_t>(v.As<Napi::Number>().Int64Value()) : dflt;
    };
    auto getU8 = [&](const char* k, uint8_t dflt) -> uint8_t
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? static_cast<uint8_t>(v.As<Napi::Number>().Uint32Value()) : dflt;
    };
    flox_runner_on_bar(_runner, sym,
                       getU8("barType", 0),
                       static_cast<uint64_t>(getInt("barTypeParam", 0)),
                       getNum("open", 0.0), getNum("high", 0.0),
                       getNum("low", 0.0), getNum("close", 0.0),
                       getNum("volume", 0.0), getNum("buyVolume", 0.0),
                       getInt("startTimeNs", 0), getInt("endTimeNs", 0),
                       getU8("closeReason", 0));
    return env.Undefined();
  }

  static void signalCb(void* ud, const FloxSignal* sig)
  {
    auto* self = static_cast<StrategyRunnerNode*>(ud);
    self->_on_signal.Call({signalToJs(self->_on_signal.Env(), sig)});
  }
};

// ──────────────────────────────────────────────────────────────
// LiveEngineNode — Disruptor-based live engine (async)
//
// Note: strategy callbacks fire from C++ consumer threads.
// In Node.js, crossing the thread boundary requires a ThreadSafeFunction.
// publish_trade / publish_book_snapshot are safe to call from JS thread.
// ──────────────────────────────────────────────────────────────

class LiveEngineNode : public Napi::ObjectWrap<LiveEngineNode>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "LiveEngine",
                       {
                           InstanceMethod("addStrategy", &LiveEngineNode::addStrategy),
                           InstanceMethod("start", &LiveEngineNode::start),
                           InstanceMethod("stop", &LiveEngineNode::stop),
                           InstanceMethod("publishTrade", &LiveEngineNode::publishTrade),
                           InstanceMethod("publishBookSnapshot", &LiveEngineNode::publishBookSnapshot),
                           InstanceMethod("publishBar", &LiveEngineNode::publishBar),
                       });
  }

  LiveEngineNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<LiveEngineNode>(info)
  {
    auto env = info.Env();
    if (info.Length() < 2)
    {
      Napi::TypeError::New(env, "LiveEngine(registry, onSignal)").ThrowAsJavaScriptException();
    }

    _reg = Napi::ObjectWrap<SymbolRegistryNode>::Unwrap(info[0].As<Napi::Object>())->get();
    _engine = flox_live_engine_create(static_cast<FloxRegistryHandle>(_reg));

    // ThreadSafeFunction: routes signal callbacks from C++ threads → Node.js event loop
    _tsfn = Napi::ThreadSafeFunction::New(
        env,
        info[1].As<Napi::Function>(),
        "flox_live_signal",
        0, 1);
  }

  ~LiveEngineNode()
  {
    if (_engine)
    {
      flox_live_engine_destroy(_engine);
    }
    _tsfn.Release();
  }

 private:
  SymbolRegistry* _reg{nullptr};
  FloxLiveEngineHandle _engine{nullptr};
  Napi::ThreadSafeFunction _tsfn;
  std::vector<std::unique_ptr<NodeStrategyHost>> _hosts;

  Napi::Value addStrategy(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    auto obj = info[0].As<Napi::Object>();
    auto symsV = obj.Get("symbols");
    std::vector<uint32_t> syms;
    if (symsV.IsArray())
    {
      auto arr = symsV.As<Napi::Array>();
      for (uint32_t i = 0; i < arr.Length(); ++i)
      {
        syms.push_back(symId(arr.Get(i)));
      }
    }
    uint32_t id = static_cast<uint32_t>(_hosts.size()) + 1;
    auto host = std::make_unique<NodeStrategyHost>(env, obj, _reg, id, syms);
    host->enableThreaded();
    flox_live_engine_add_strategy(_engine,
                                  static_cast<FloxStrategyHandle>(host->bridge.get()),
                                  &LiveEngineNode::signalCb, this);
    _hosts.push_back(std::move(host));
    return env.Undefined();
  }

  Napi::Value start(const Napi::CallbackInfo& info)
  {
    flox_live_engine_start(_engine);
    return info.Env().Undefined();
  }

  Napi::Value stop(const Napi::CallbackInfo& info)
  {
    flox_live_engine_stop(_engine);
    return info.Env().Undefined();
  }

  Napi::Value publishTrade(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    double price = info[1].As<Napi::Number>().DoubleValue();
    double qty = info[2].As<Napi::Number>().DoubleValue();
    bool isBuy = info[3].As<Napi::Boolean>().Value();
    int64_t ts = info.Length() > 4 ? static_cast<int64_t>(info[4].As<Napi::Number>().Int64Value()) : 0;
    flox_live_engine_publish_trade(_engine, sym, price, qty, isBuy ? 1 : 0, ts);
    return info.Env().Undefined();
  }

  Napi::Value publishBookSnapshot(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    auto bidP = info[1].As<Napi::Array>();
    auto bidQ = info[2].As<Napi::Array>();
    auto askP = info[3].As<Napi::Array>();
    auto askQ = info[4].As<Napi::Array>();
    int64_t ts = info.Length() > 5 ? static_cast<int64_t>(info[5].As<Napi::Number>().Int64Value()) : 0;

    auto toVec = [](const Napi::Array& a)
    {
      std::vector<double> v(a.Length());
      for (uint32_t i = 0; i < a.Length(); ++i)
      {
        v[i] = a.Get(i).As<Napi::Number>().DoubleValue();
      }
      return v;
    };
    auto bp = toVec(bidP), bq = toVec(bidQ), ap = toVec(askP), aq = toVec(askQ);
    flox_live_engine_publish_book_snapshot(_engine, sym,
                                           bp.data(), bq.data(), static_cast<uint32_t>(bp.size()),
                                           ap.data(), aq.data(), static_cast<uint32_t>(ap.size()), ts);
    return info.Env().Undefined();
  }

  // publishBar(symbol, { open, high, low, close, volume?, buyVolume?,
  //                      startTimeNs?, endTimeNs?, barType?, barTypeParam?,
  //                      closeReason? })
  Napi::Value publishBar(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    auto opts = info[1].As<Napi::Object>();
    auto getNum = [&](const char* k, double dflt) -> double
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? v.As<Napi::Number>().DoubleValue() : dflt;
    };
    auto getInt = [&](const char* k, int64_t dflt) -> int64_t
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? static_cast<int64_t>(v.As<Napi::Number>().Int64Value()) : dflt;
    };
    auto getU8 = [&](const char* k, uint8_t dflt) -> uint8_t
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? static_cast<uint8_t>(v.As<Napi::Number>().Uint32Value()) : dflt;
    };
    flox_live_engine_publish_bar(_engine, sym,
                                 getU8("barType", 0),
                                 static_cast<uint64_t>(getInt("barTypeParam", 0)),
                                 getNum("open", 0.0), getNum("high", 0.0),
                                 getNum("low", 0.0), getNum("close", 0.0),
                                 getNum("volume", 0.0), getNum("buyVolume", 0.0),
                                 getInt("startTimeNs", 0), getInt("endTimeNs", 0),
                                 getU8("closeReason", 0));
    return info.Env().Undefined();
  }

  static void signalCb(void* ud, const FloxSignal* sig)
  {
    auto* self = static_cast<LiveEngineNode*>(ud);
    // Heap-allocate the copy — the lambda owns it and deletes after use.
    auto* copy = new FloxSignal(*sig);
    self->_tsfn.NonBlockingCall(copy, [](Napi::Env env, Napi::Function fn, FloxSignal* s)
                                {
      fn.Call({signalToJs(env, s)});
      delete s; });
  }
};

// ──────────────────────────────────────────────────────────────
// BacktestRunnerNode
// ──────────────────────────────────────────────────────────────

class BacktestRunnerNode : public Napi::ObjectWrap<BacktestRunnerNode>
{
 public:
  static Napi::Object Init(Napi::Env env)
  {
    return DefineClass(env, "BacktestRunner",
                       {
                           InstanceMethod("setStrategy", &BacktestRunnerNode::setStrategy),
                           InstanceMethod("runCsv", &BacktestRunnerNode::runCsv),
                           InstanceMethod("runOhlcv", &BacktestRunnerNode::runOhlcv),
                           InstanceMethod("runBars", &BacktestRunnerNode::runBars),
                       });
  }

  // new BacktestRunner(registry, feeRate, initialCapital)
  BacktestRunnerNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BacktestRunnerNode>(info)
  {
    _reg = Napi::ObjectWrap<SymbolRegistryNode>::Unwrap(info[0].As<Napi::Object>())->get();
    double fee = info[1].As<Napi::Number>().DoubleValue();
    double cap = info[2].As<Napi::Number>().DoubleValue();
    _handle = flox_backtest_runner_create(static_cast<FloxRegistryHandle>(_reg), fee, cap);
  }

  ~BacktestRunnerNode()
  {
    if (_handle)
    {
      flox_backtest_runner_destroy(_handle);
    }
  }

 private:
  FloxBacktestRunnerHandle _handle{nullptr};
  SymbolRegistry* _reg{nullptr};
  std::unique_ptr<NodeStrategyHost> _host;

  // runner.setStrategy(strategyObj) — plain JS object with callbacks
  Napi::Value setStrategy(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    auto obj = info[0].As<Napi::Object>();
    std::vector<uint32_t> syms;
    auto symsV = obj.Get("symbols");
    if (symsV.IsArray())
    {
      auto arr = symsV.As<Napi::Array>();
      for (uint32_t i = 0; i < arr.Length(); ++i)
      {
        syms.push_back(symId(arr.Get(i)));
      }
    }
    _host = std::make_unique<NodeStrategyHost>(env, obj, _reg, 1, syms);
    flox_backtest_runner_set_strategy(_handle,
                                      static_cast<FloxStrategyHandle>(_host->bridge.get()));
    return env.Undefined();
  }

  // runner.runCsv(path, symbol) → stats object
  Napi::Value runCsv(const Napi::CallbackInfo& info)
  {
    std::string path = info[0].As<Napi::String>().Utf8Value();
    std::string symbol = info[1].As<Napi::String>().Utf8Value();
    FloxBacktestStats s{};
    int ok = flox_backtest_runner_run_csv(_handle, path.c_str(), symbol.c_str(), &s);
    if (!ok)
    {
      return info.Env().Null();
    }
    return statsToJs(info.Env(), s);
  }

  // runner.runOhlcv(tsArray, closeArray, symbol) → stats object
  Napi::Value runOhlcv(const Napi::CallbackInfo& info)
  {
    auto tsArr = info[0].As<Napi::BigInt64Array>();
    auto closeArr = info[1].As<Napi::Float64Array>();
    std::string symbol = info[2].As<Napi::String>().Utf8Value();
    uint32_t n = static_cast<uint32_t>(tsArr.ElementLength());
    FloxBacktestStats s{};
    int ok = flox_backtest_runner_run_ohlcv(_handle,
                                            reinterpret_cast<const int64_t*>(tsArr.Data()),
                                            closeArr.Data(),
                                            n, symbol.c_str(), &s);
    if (!ok)
    {
      return info.Env().Null();
    }
    return statsToJs(info.Env(), s);
  }

  // runner.runBars(startNs, endNs, open, high, low, close, volume,
  //                symbol, barType?, barTypeParam?) → stats
  // All arrays are typed (BigInt64Array for ts, Float64Array for OHLCV).
  Napi::Value runBars(const Napi::CallbackInfo& info)
  {
    auto startNs = info[0].As<Napi::BigInt64Array>();
    auto endNs = info[1].As<Napi::BigInt64Array>();
    auto openA = info[2].As<Napi::Float64Array>();
    auto highA = info[3].As<Napi::Float64Array>();
    auto lowA = info[4].As<Napi::Float64Array>();
    auto closeA = info[5].As<Napi::Float64Array>();
    auto volA = info[6].As<Napi::Float64Array>();
    std::string symbol = info[7].As<Napi::String>().Utf8Value();
    uint8_t barType = info.Length() > 8 ? static_cast<uint8_t>(info[8].As<Napi::Number>().Uint32Value()) : 0;
    uint64_t barTypeParam = info.Length() > 9
                                ? static_cast<uint64_t>(info[9].As<Napi::Number>().Int64Value())
                                : 0;
    uint32_t n = static_cast<uint32_t>(startNs.ElementLength());
    FloxBacktestStats s{};
    int ok = flox_backtest_runner_run_bars(
        _handle,
        reinterpret_cast<const int64_t*>(startNs.Data()),
        reinterpret_cast<const int64_t*>(endNs.Data()),
        openA.Data(), highA.Data(), lowA.Data(), closeA.Data(), volA.Data(),
        n, symbol.c_str(), barType, barTypeParam, &s);
    if (!ok)
    {
      return info.Env().Null();
    }
    return statsToJs(info.Env(), s);
  }

  static Napi::Object statsToJs(Napi::Env env, const FloxBacktestStats& s)
  {
    auto obj = Napi::Object::New(env);
    obj.Set("totalTrades", Napi::Number::New(env, static_cast<double>(s.totalTrades)));
    obj.Set("winningTrades", Napi::Number::New(env, static_cast<double>(s.winningTrades)));
    obj.Set("losingTrades", Napi::Number::New(env, static_cast<double>(s.losingTrades)));
    obj.Set("initialCapital", Napi::Number::New(env, s.initialCapital));
    obj.Set("finalCapital", Napi::Number::New(env, s.finalCapital));
    obj.Set("netPnl", Napi::Number::New(env, s.netPnl));
    obj.Set("totalFees", Napi::Number::New(env, s.totalFees));
    obj.Set("returnPct", Napi::Number::New(env, s.returnPct));
    obj.Set("maxDrawdown", Napi::Number::New(env, s.maxDrawdown));
    obj.Set("maxDrawdownPct", Napi::Number::New(env, s.maxDrawdownPct));
    obj.Set("sharpeRatio", Napi::Number::New(env, s.sharpeRatio));
    obj.Set("winRate", Napi::Number::New(env, s.winRate));
    obj.Set("avgWin", Napi::Number::New(env, s.avgWin));
    obj.Set("avgLoss", Napi::Number::New(env, s.avgLoss));
    obj.Set("profitFactor", Napi::Number::New(env, s.profitFactor));
    return obj;
  }
};

// ──────────────────────────────────────────────────────────────
// RunnerNode — unified live runner.
//
//   new Runner(registry, onSignal)                — sync
//   new Runner(registry, onSignal, true)          — Disruptor (threaded)
// ──────────────────────────────────────────────────────────────

class RunnerNode : public Napi::ObjectWrap<RunnerNode>
{
 public:
  static Napi::Object Init(Napi::Env env)
  {
    return DefineClass(env, "Runner",
                       {
                           InstanceMethod("addStrategy", &RunnerNode::addStrategy),
                           InstanceMethod("start", &RunnerNode::start),
                           InstanceMethod("stop", &RunnerNode::stop),
                           InstanceMethod("onTrade", &RunnerNode::onTrade),
                           InstanceMethod("onBookSnapshot", &RunnerNode::onBookSnapshot),
                           InstanceMethod("onBar", &RunnerNode::onBar),
                       });
  }

  RunnerNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<RunnerNode>(info)
  {
    auto env = info.Env();
    auto* reg = Napi::ObjectWrap<SymbolRegistryNode>::Unwrap(info[0].As<Napi::Object>())->get();
    auto onSig = info[1].As<Napi::Function>();
    bool threaded = info.Length() > 2 && info[2].As<Napi::Boolean>().Value();

    if (threaded)
    {
      _reg = reg;
      _engine = flox_live_engine_create(static_cast<FloxRegistryHandle>(reg));
      _tsfn = Napi::ThreadSafeFunction::New(env, onSig, "flox_runner_signal", 0, 1);
      _mode = Mode::Threaded;
    }
    else
    {
      _reg = reg;
      _runner = flox_runner_create(static_cast<FloxRegistryHandle>(reg),
                                   &RunnerNode::signalCbSync, this);
      _on_signal = Napi::Persistent(onSig);
      _mode = Mode::Sync;
    }
  }

  ~RunnerNode()
  {
    if (_mode == Mode::Sync && _runner)
    {
      flox_runner_destroy(_runner);
    }
    if (_mode == Mode::Threaded && _engine)
    {
      flox_live_engine_destroy(_engine);
    }
    if (_mode == Mode::Threaded)
    {
      _tsfn.Release();
    }
  }

 private:
  enum class Mode
  {
    Sync,
    Threaded
  };
  Mode _mode{Mode::Sync};
  SymbolRegistry* _reg{nullptr};
  // Sync
  FloxRunnerHandle _runner{nullptr};
  Napi::FunctionReference _on_signal;
  // Threaded
  FloxLiveEngineHandle _engine{nullptr};
  Napi::ThreadSafeFunction _tsfn;

  std::vector<std::unique_ptr<NodeStrategyHost>> _hosts;

  std::vector<uint32_t> extractSyms(Napi::Object obj)
  {
    std::vector<uint32_t> syms;
    auto symsV = obj.Get("symbols");
    if (symsV.IsArray())
    {
      auto arr = symsV.As<Napi::Array>();
      for (uint32_t i = 0; i < arr.Length(); ++i)
      {
        syms.push_back(symId(arr.Get(i)));
      }
    }
    return syms;
  }

  Napi::Value addStrategy(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    auto obj = info[0].As<Napi::Object>();
    uint32_t id = static_cast<uint32_t>(_hosts.size()) + 1;
    auto host = std::make_unique<NodeStrategyHost>(env, obj, _reg, id, extractSyms(obj));
    if (_mode == Mode::Threaded)
    {
      host->enableThreaded();
      flox_live_engine_add_strategy(_engine,
                                    static_cast<FloxStrategyHandle>(host->bridge.get()),
                                    &RunnerNode::signalCbThreaded, this);
    }
    else
    {
      flox_runner_add_strategy(_runner,
                               static_cast<FloxStrategyHandle>(host->bridge.get()));
    }
    _hosts.push_back(std::move(host));
    return env.Undefined();
  }

  Napi::Value start(const Napi::CallbackInfo& info)
  {
    if (_mode == Mode::Sync)
    {
      flox_runner_start(_runner);
    }
    else
    {
      flox_live_engine_start(_engine);
    }
    return info.Env().Undefined();
  }

  Napi::Value stop(const Napi::CallbackInfo& info)
  {
    if (_mode == Mode::Sync)
    {
      flox_runner_stop(_runner);
    }
    else
    {
      flox_live_engine_stop(_engine);
    }
    return info.Env().Undefined();
  }

  Napi::Value onTrade(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    double price = info[1].As<Napi::Number>().DoubleValue();
    double qty = info[2].As<Napi::Number>().DoubleValue();
    bool isBuy = info[3].As<Napi::Boolean>().Value();
    int64_t ts = info.Length() > 4 ? static_cast<int64_t>(info[4].As<Napi::Number>().Int64Value()) : 0;
    if (_mode == Mode::Sync)
    {
      flox_runner_on_trade(_runner, sym, price, qty, isBuy ? 1 : 0, ts);
    }
    else
    {
      flox_live_engine_publish_trade(_engine, sym, price, qty, isBuy ? 1 : 0, ts);
    }
    return info.Env().Undefined();
  }

  Napi::Value onBookSnapshot(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    auto bidP = info[1].As<Napi::Array>();
    auto bidQ = info[2].As<Napi::Array>();
    auto askP = info[3].As<Napi::Array>();
    auto askQ = info[4].As<Napi::Array>();
    int64_t ts = info.Length() > 5 ? static_cast<int64_t>(info[5].As<Napi::Number>().Int64Value()) : 0;
    auto toVec = [](const Napi::Array& a)
    {
      std::vector<double> v(a.Length());
      for (uint32_t i = 0; i < a.Length(); ++i)
      {
        v[i] = a.Get(i).As<Napi::Number>().DoubleValue();
      }
      return v;
    };
    auto bp = toVec(bidP), bq = toVec(bidQ), ap = toVec(askP), aq = toVec(askQ);
    if (_mode == Mode::Sync)
    {
      flox_runner_on_book_snapshot(_runner, sym,
                                   bp.data(), bq.data(), static_cast<uint32_t>(bp.size()),
                                   ap.data(), aq.data(), static_cast<uint32_t>(ap.size()), ts);
    }
    else
    {
      flox_live_engine_publish_book_snapshot(_engine, sym,
                                             bp.data(), bq.data(), static_cast<uint32_t>(bp.size()),
                                             ap.data(), aq.data(), static_cast<uint32_t>(ap.size()), ts);
    }
    return info.Env().Undefined();
  }

  Napi::Value onBar(const Napi::CallbackInfo& info)
  {
    uint32_t sym = symId(info[0]);
    auto opts = info[1].As<Napi::Object>();
    auto getNum = [&](const char* k, double dflt) -> double
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? v.As<Napi::Number>().DoubleValue() : dflt;
    };
    auto getInt = [&](const char* k, int64_t dflt) -> int64_t
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? static_cast<int64_t>(v.As<Napi::Number>().Int64Value()) : dflt;
    };
    auto getU8 = [&](const char* k, uint8_t dflt) -> uint8_t
    {
      auto v = opts.Get(k);
      return v.IsNumber() ? static_cast<uint8_t>(v.As<Napi::Number>().Uint32Value()) : dflt;
    };
    uint8_t bt = getU8("barType", 0);
    uint64_t btp = static_cast<uint64_t>(getInt("barTypeParam", 0));
    double o = getNum("open", 0.0);
    double h = getNum("high", 0.0);
    double l = getNum("low", 0.0);
    double c = getNum("close", 0.0);
    double v = getNum("volume", 0.0);
    double bv = getNum("buyVolume", 0.0);
    int64_t sNs = getInt("startTimeNs", 0);
    int64_t eNs = getInt("endTimeNs", 0);
    uint8_t cr = getU8("closeReason", 0);
    if (_mode == Mode::Sync)
    {
      flox_runner_on_bar(_runner, sym, bt, btp, o, h, l, c, v, bv, sNs, eNs, cr);
    }
    else
    {
      flox_live_engine_publish_bar(_engine, sym, bt, btp, o, h, l, c, v, bv, sNs, eNs, cr);
    }
    return info.Env().Undefined();
  }

  static void signalCbSync(void* ud, const FloxSignal* sig)
  {
    auto* self = static_cast<RunnerNode*>(ud);
    self->_on_signal.Call({signalToJs(self->_on_signal.Env(), sig)});
  }

  static void signalCbThreaded(void* ud, const FloxSignal* sig)
  {
    auto* self = static_cast<RunnerNode*>(ud);
    auto* copy = new FloxSignal(*sig);
    self->_tsfn.NonBlockingCall(copy, [](Napi::Env env, Napi::Function fn, FloxSignal* s)
                                {
      fn.Call({signalToJs(env, s)});
      delete s; });
  }
};

inline void registerStrategy(Napi::Env env, Napi::Object exports)
{
  exports.Set("SymbolRegistry", SymbolRegistryNode::Init(env));
  exports.Set("Runner", RunnerNode::Init(env));
  exports.Set("BacktestRunner", BacktestRunnerNode::Init(env));
}

}  // namespace node_flox
