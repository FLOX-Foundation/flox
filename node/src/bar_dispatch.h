// node/src/bar_dispatch.h — bar-close dispatch recorder NAPI wrap.
//
// Cross-binding parity test fixture for the documented bar-close
// ordering rule: on tied closes, MultiTimeframeAggregator dispatches
// bars in the order their timeframes were registered. The wrap is a
// thin shim over the C ABI flox_bar_dispatch_recorder_* surface.

#pragma once

#include <napi.h>

#include "flox/capi/flox_capi.h"

namespace node_flox
{

class BarDispatchRecorderWrap : public Napi::ObjectWrap<BarDispatchRecorderWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "BarDispatchRecorder",
        {InstanceMethod("addTimeIntervalSeconds",
                        &BarDispatchRecorderWrap::AddTimeIntervalSeconds),
         InstanceMethod("onTrade", &BarDispatchRecorderWrap::OnTrade),
         InstanceMethod("finalize", &BarDispatchRecorderWrap::Finalize),
         InstanceMethod("count", &BarDispatchRecorderWrap::Count),
         InstanceMethod("typeAt", &BarDispatchRecorderWrap::TypeAt),
         InstanceMethod("paramAt", &BarDispatchRecorderWrap::ParamAt)});
  }

  BarDispatchRecorderWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BarDispatchRecorderWrap>(info)
  {
    _h = flox_bar_dispatch_recorder_create();
  }

  ~BarDispatchRecorderWrap()
  {
    if (_h)
    {
      flox_bar_dispatch_recorder_destroy(_h);
      _h = nullptr;
    }
  }

 private:
  Napi::Value AddTimeIntervalSeconds(const Napi::CallbackInfo& info)
  {
    uint32_t seconds = info[0].As<Napi::Number>().Uint32Value();
    return Napi::Number::New(info.Env(),
                             flox_bar_dispatch_recorder_add_time_seconds(_h, seconds));
  }

  void OnTrade(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = info[0].As<Napi::Number>().Uint32Value();
    double price = info[1].As<Napi::Number>().DoubleValue();
    double qty = info[2].As<Napi::Number>().DoubleValue();
    int64_t ts_ns = info[3].As<Napi::Number>().Int64Value();
    flox_bar_dispatch_recorder_on_trade(_h, symbol, price, qty, ts_ns);
  }

  void Finalize(const Napi::CallbackInfo&) { flox_bar_dispatch_recorder_finalize(_h); }

  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_bar_dispatch_recorder_count(_h));
  }

  Napi::Value TypeAt(const Napi::CallbackInfo& info)
  {
    uint32_t i = info[0].As<Napi::Number>().Uint32Value();
    return Napi::Number::New(info.Env(), flox_bar_dispatch_recorder_type_at(_h, i));
  }

  Napi::Value ParamAt(const Napi::CallbackInfo& info)
  {
    uint32_t i = info[0].As<Napi::Number>().Uint32Value();
    return Napi::Number::New(info.Env(),
                             static_cast<double>(flox_bar_dispatch_recorder_param_at(_h, i)));
  }

  FloxBarDispatchRecorderHandle _h = nullptr;
};

inline void registerBarDispatchRecorder(Napi::Env env, Napi::Object exports)
{
  exports.Set("BarDispatchRecorder", BarDispatchRecorderWrap::Init(env));
}

}  // namespace node_flox
