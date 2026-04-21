// node/src/books.h -- OrderBook, L3Book

#pragma once
#include <napi.h>
#include "flox/capi/flox_capi.h"

namespace node_flox
{

class OrderBookWrap : public Napi::ObjectWrap<OrderBookWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "OrderBook",
      {InstanceMethod("applySnapshot", &OrderBookWrap::ApplySnapshot),
       InstanceMethod("applyDelta", &OrderBookWrap::ApplyDelta),
       InstanceMethod("bestBid", &OrderBookWrap::BestBid),
       InstanceMethod("bestAsk", &OrderBookWrap::BestAsk),
       InstanceMethod("mid", &OrderBookWrap::Mid),
       InstanceMethod("spread", &OrderBookWrap::Spread),
       InstanceMethod("isCrossed", &OrderBookWrap::IsCrossed),
       InstanceMethod("getBids", &OrderBookWrap::GetBids),
       InstanceMethod("getAsks", &OrderBookWrap::GetAsks),
       InstanceMethod("clear", &OrderBookWrap::Clear)});
  }
  OrderBookWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<OrderBookWrap>(info),
    _h(flox_book_create(info[0].As<Napi::Number>().DoubleValue())) {}
  ~OrderBookWrap() { if (_h) flox_book_destroy(_h); }

 private:
  void applyUpdate(const Napi::CallbackInfo& info,
                   void (*fn)(FloxBookHandle, const double*, const double*, size_t, const double*, const double*, size_t))
  {
    auto bp = info[0].As<Napi::Float64Array>();
    auto bq = info[1].As<Napi::Float64Array>();
    auto ap = info[2].As<Napi::Float64Array>();
    auto aq = info[3].As<Napi::Float64Array>();
    fn(_h, bp.Data(), bq.Data(), bp.ElementLength(), ap.Data(), aq.Data(), ap.ElementLength());
  }
  void ApplySnapshot(const Napi::CallbackInfo& info) { applyUpdate(info, flox_book_apply_snapshot); }
  void ApplyDelta(const Napi::CallbackInfo& info) { applyUpdate(info, flox_book_apply_delta); }

  Napi::Value optDouble(const Napi::CallbackInfo& info, uint8_t(*fn)(FloxBookHandle, double*))
  {
    double v = 0;
    if (fn(_h, &v)) return Napi::Number::New(info.Env(), v);
    return info.Env().Null();
  }
  Napi::Value BestBid(const Napi::CallbackInfo& info) { return optDouble(info, flox_book_best_bid); }
  Napi::Value BestAsk(const Napi::CallbackInfo& info) { return optDouble(info, flox_book_best_ask); }
  Napi::Value Mid(const Napi::CallbackInfo& info) { return optDouble(info, flox_book_mid); }
  Napi::Value Spread(const Napi::CallbackInfo& info) { return optDouble(info, flox_book_spread); }
  Napi::Value IsCrossed(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), flox_book_is_crossed(_h)); }
  void Clear(const Napi::CallbackInfo&) { flox_book_clear(_h); }

  Napi::Value getLevels(const Napi::CallbackInfo& info, uint32_t(*fn)(FloxBookHandle, double*, double*, uint32_t))
  {
    uint32_t n = info[0].As<Napi::Number>().Uint32Value();
    if (n > 100) n = 100;
    double prices[100], qtys[100];
    uint32_t got = fn(_h, prices, qtys, n);
    auto arr = Napi::Array::New(info.Env(), got);
    for (uint32_t i = 0; i < got; i++) {
      auto lv = Napi::Array::New(info.Env(), 2);
      lv.Set((uint32_t)0, prices[i]); lv.Set((uint32_t)1, qtys[i]);
      arr.Set(i, lv);
    }
    return arr;
  }
  Napi::Value GetBids(const Napi::CallbackInfo& info) { return getLevels(info, flox_book_get_bids); }
  Napi::Value GetAsks(const Napi::CallbackInfo& info) { return getLevels(info, flox_book_get_asks); }

  FloxBookHandle _h;
};

class L3BookWrap : public Napi::ObjectWrap<L3BookWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "L3Book",
      {InstanceMethod("addOrder", &L3BookWrap::AddOrder),
       InstanceMethod("removeOrder", &L3BookWrap::RemoveOrder),
       InstanceMethod("modifyOrder", &L3BookWrap::ModifyOrder),
       InstanceMethod("bestBid", &L3BookWrap::BestBid),
       InstanceMethod("bestAsk", &L3BookWrap::BestAsk),
       InstanceMethod("bidAtPrice", &L3BookWrap::BidAtPrice),
       InstanceMethod("askAtPrice", &L3BookWrap::AskAtPrice)});
  }
  L3BookWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<L3BookWrap>(info), _h(flox_l3_book_create()) {}
  ~L3BookWrap() { if (_h) flox_l3_book_destroy(_h); }

 private:
  Napi::Value AddOrder(const Napi::CallbackInfo& info) {
    std::string side = info[3].As<Napi::String>().Utf8Value();
    return Napi::Number::New(info.Env(), flox_l3_book_add_order(_h,
      info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().DoubleValue(),
      info[2].As<Napi::Number>().DoubleValue(), side == "buy" ? 0 : 1));
  }
  Napi::Value RemoveOrder(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_l3_book_remove_order(_h, info[0].As<Napi::Number>().Int64Value())); }
  Napi::Value ModifyOrder(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_l3_book_modify_order(_h, info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().DoubleValue())); }
  Napi::Value BestBid(const Napi::CallbackInfo& info) { double v; if (flox_l3_book_best_bid(_h, &v)) return Napi::Number::New(info.Env(), v); return info.Env().Null(); }
  Napi::Value BestAsk(const Napi::CallbackInfo& info) { double v; if (flox_l3_book_best_ask(_h, &v)) return Napi::Number::New(info.Env(), v); return info.Env().Null(); }
  Napi::Value BidAtPrice(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_l3_book_bid_at_price(_h, info[0].As<Napi::Number>().DoubleValue())); }
  Napi::Value AskAtPrice(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_l3_book_ask_at_price(_h, info[0].As<Napi::Number>().DoubleValue())); }
  FloxL3BookHandle _h;
};

class CompositeBookMatrixWrap : public Napi::ObjectWrap<CompositeBookMatrixWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "CompositeBookMatrix",
      {InstanceMethod("bestBid", &CompositeBookMatrixWrap::BestBid),
       InstanceMethod("bestAsk", &CompositeBookMatrixWrap::BestAsk),
       InstanceMethod("hasArbitrage", &CompositeBookMatrixWrap::HasArb),
       InstanceMethod("markStale", &CompositeBookMatrixWrap::MarkStale),
       InstanceMethod("checkStaleness", &CompositeBookMatrixWrap::CheckStaleness)});
  }
  CompositeBookMatrixWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<CompositeBookMatrixWrap>(info), _h(flox_composite_book_create()) {}
  ~CompositeBookMatrixWrap() { if (_h) flox_composite_book_destroy(_h); }
 private:
  Napi::Value BestBid(const Napi::CallbackInfo& info) {
    double p=0,q=0; if (flox_composite_book_best_bid(_h, info[0].As<Napi::Number>().Uint32Value(), &p, &q)) { auto o=Napi::Object::New(info.Env()); o.Set("price",p); o.Set("qty",q); return o; } return info.Env().Null();
  }
  Napi::Value BestAsk(const Napi::CallbackInfo& info) {
    double p=0,q=0; if (flox_composite_book_best_ask(_h, info[0].As<Napi::Number>().Uint32Value(), &p, &q)) { auto o=Napi::Object::New(info.Env()); o.Set("price",p); o.Set("qty",q); return o; } return info.Env().Null();
  }
  Napi::Value HasArb(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), flox_composite_book_has_arb(_h, info[0].As<Napi::Number>().Uint32Value())); }
  void MarkStale(const Napi::CallbackInfo& info) { flox_composite_book_mark_stale(_h, info[0].As<Napi::Number>().Uint32Value(), info[1].As<Napi::Number>().Uint32Value()); }
  void CheckStaleness(const Napi::CallbackInfo& info) { flox_composite_book_check_staleness(_h, info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().Int64Value()); }
  FloxCompositeBookHandle _h;
};

inline void registerBooks(Napi::Env env, Napi::Object exports)
{
  exports.Set("OrderBook", OrderBookWrap::Init(env));
  exports.Set("L3Book", L3BookWrap::Init(env));
  exports.Set("CompositeBookMatrix", CompositeBookMatrixWrap::Init(env));
}

}  // namespace node_flox
