// node/src/positions.h -- PositionTracker, PositionGroupTracker, OrderTracker

#pragma once
#include <napi.h>
#include "flox/capi/flox_capi.h"

namespace node_flox
{

class PositionTrackerWrap : public Napi::ObjectWrap<PositionTrackerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "PositionTracker",
                       {InstanceMethod("onFill", &PositionTrackerWrap::OnFill),
                        InstanceMethod("position", &PositionTrackerWrap::Position),
                        InstanceMethod("avgEntryPrice", &PositionTrackerWrap::AvgEntry),
                        InstanceMethod("realizedPnl", &PositionTrackerWrap::Pnl),
                        InstanceMethod("totalRealizedPnl", &PositionTrackerWrap::TotalPnl)});
  }
  PositionTrackerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<PositionTrackerWrap>(info),
                                                        _h(flox_position_tracker_create(info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0)) {}
  ~PositionTrackerWrap()
  {
    if (_h)
    {
      flox_position_tracker_destroy(_h);
    }
  }

 private:
  void OnFill(const Napi::CallbackInfo& info)
  {
    std::string side = info[1].As<Napi::String>().Utf8Value();
    flox_position_tracker_on_fill(_h, info[0].As<Napi::Number>().Uint32Value(),
                                  side == "buy" ? 0 : 1, info[2].As<Napi::Number>().DoubleValue(), info[3].As<Napi::Number>().DoubleValue());
  }
  Napi::Value Position(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_position_tracker_position(_h, info[0].As<Napi::Number>().Uint32Value())); }
  Napi::Value AvgEntry(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_position_tracker_avg_entry(_h, info[0].As<Napi::Number>().Uint32Value())); }
  Napi::Value Pnl(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_position_tracker_realized_pnl(_h, info[0].As<Napi::Number>().Uint32Value())); }
  Napi::Value TotalPnl(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_position_tracker_total_pnl(_h)); }
  FloxPositionTrackerHandle _h;
};

class PositionGroupTrackerWrap : public Napi::ObjectWrap<PositionGroupTrackerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "PositionGroupTracker",
                       {InstanceMethod("openPosition", &PositionGroupTrackerWrap::Open),
                        InstanceMethod("closePosition", &PositionGroupTrackerWrap::Close),
                        InstanceMethod("partialClose", &PositionGroupTrackerWrap::PartialClose),
                        InstanceMethod("netPosition", &PositionGroupTrackerWrap::Net),
                        InstanceMethod("realizedPnl", &PositionGroupTrackerWrap::Pnl),
                        InstanceMethod("totalRealizedPnl", &PositionGroupTrackerWrap::TotalPnl),
                        InstanceMethod("openCount", &PositionGroupTrackerWrap::OpenCount),
                        InstanceMethod("prune", &PositionGroupTrackerWrap::Prune)});
  }
  PositionGroupTrackerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<PositionGroupTrackerWrap>(info), _h(flox_position_group_create()) {}
  ~PositionGroupTrackerWrap()
  {
    if (_h)
    {
      flox_position_group_destroy(_h);
    }
  }

 private:
  Napi::Value Open(const Napi::CallbackInfo& info)
  {
    std::string side = info[2].As<Napi::String>().Utf8Value();
    return Napi::Number::New(info.Env(), (double)flox_position_group_open(_h,
                                                                          info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().Uint32Value(),
                                                                          side == "buy" ? 0 : 1, info[3].As<Napi::Number>().DoubleValue(), info[4].As<Napi::Number>().DoubleValue()));
  }
  void Close(const Napi::CallbackInfo& info) { flox_position_group_close(_h, info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().DoubleValue()); }
  void PartialClose(const Napi::CallbackInfo& info) { flox_position_group_partial_close(_h, info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().DoubleValue(), info[2].As<Napi::Number>().DoubleValue()); }
  Napi::Value Net(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_position_group_net_position(_h, info[0].As<Napi::Number>().Uint32Value())); }
  Napi::Value Pnl(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_position_group_realized_pnl(_h, info[0].As<Napi::Number>().Uint32Value())); }
  Napi::Value TotalPnl(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_position_group_total_pnl(_h)); }
  Napi::Value OpenCount(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_position_group_open_count(_h, info[0].As<Napi::Number>().Uint32Value())); }
  void Prune(const Napi::CallbackInfo&) { flox_position_group_prune(_h); }
  FloxPositionGroupHandle _h;
};

class OrderTrackerWrap : public Napi::ObjectWrap<OrderTrackerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "OrderTracker",
                       {InstanceMethod("onSubmitted", &OrderTrackerWrap::OnSubmitted),
                        InstanceMethod("onFilled", &OrderTrackerWrap::OnFilled),
                        InstanceMethod("onCanceled", &OrderTrackerWrap::OnCanceled),
                        InstanceMethod("isActive", &OrderTrackerWrap::IsActive),
                        InstanceAccessor("activeCount", &OrderTrackerWrap::ActiveCount, nullptr),
                        InstanceAccessor("totalCount", &OrderTrackerWrap::TotalCount, nullptr),
                        InstanceMethod("prune", &OrderTrackerWrap::Prune)});
  }
  OrderTrackerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<OrderTrackerWrap>(info), _h(flox_order_tracker_create()) {}
  ~OrderTrackerWrap()
  {
    if (_h)
    {
      flox_order_tracker_destroy(_h);
    }
  }

 private:
  Napi::Value OnSubmitted(const Napi::CallbackInfo& info)
  {
    std::string side = info[2].As<Napi::String>().Utf8Value();
    return Napi::Boolean::New(info.Env(), flox_order_tracker_on_submitted(_h,
                                                                          info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().Uint32Value(),
                                                                          side == "buy" ? 0 : 1, info[3].As<Napi::Number>().DoubleValue(), info[4].As<Napi::Number>().DoubleValue()));
  }
  Napi::Value OnFilled(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), flox_order_tracker_on_filled(_h, info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().DoubleValue())); }
  Napi::Value OnCanceled(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), flox_order_tracker_on_canceled(_h, info[0].As<Napi::Number>().Int64Value())); }
  Napi::Value IsActive(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), flox_order_tracker_is_active(_h, info[0].As<Napi::Number>().Int64Value())); }
  Napi::Value ActiveCount(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_order_tracker_active_count(_h)); }
  Napi::Value TotalCount(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_order_tracker_total_count(_h)); }
  void Prune(const Napi::CallbackInfo&) { flox_order_tracker_prune(_h); }
  FloxOrderTrackerHandle _h;
};

inline void registerPositions(Napi::Env env, Napi::Object exports)
{
  exports.Set("PositionTracker", PositionTrackerWrap::Init(env));
  exports.Set("PositionGroupTracker", PositionGroupTrackerWrap::Init(env));
  exports.Set("OrderTracker", OrderTrackerWrap::Init(env));
  exports.Set("POSITION_FIFO", Napi::Number::New(env, 0));
  exports.Set("POSITION_AVG_COST", Napi::Number::New(env, 1));
}

}  // namespace node_flox
