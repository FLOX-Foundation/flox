// node/src/liquidation_engine.h — Liquidation + insurance + ADL wrap.

#pragma once

#include <napi.h>

#include <string>

#include "flox/capi/flox_capi.h"

namespace node_flox
{

class LiquidationEngineWrap : public Napi::ObjectWrap<LiquidationEngineWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "LiquidationEngine",
        {InstanceMethod("addTier", &LiquidationEngineWrap::AddTier),
         InstanceMethod("setInsuranceFundCapital",
                        &LiquidationEngineWrap::SetInsuranceFundCapital),
         InstanceMethod("insuranceFundBalance",
                        &LiquidationEngineWrap::InsuranceFundBalance),
         InstanceMethod("setAdlEnabled", &LiquidationEngineWrap::SetAdlEnabled),
         InstanceMethod("setLiquidationSlippageBps",
                        &LiquidationEngineWrap::SetLiquidationSlippageBps),
         InstanceMethod("openPosition", &LiquidationEngineWrap::OpenPosition),
         InstanceMethod("closePosition", &LiquidationEngineWrap::ClosePosition),
         InstanceMethod("onMark", &LiquidationEngineWrap::OnMark),
         InstanceMethod("liquidationsCount",
                        &LiquidationEngineWrap::LiquidationsCount),
         InstanceMethod("insurancePaymentsCount",
                        &LiquidationEngineWrap::InsurancePaymentsCount),
         InstanceMethod("adlCloseoutsCount",
                        &LiquidationEngineWrap::AdlCloseoutsCount),
         InstanceMethod("loadProfile", &LiquidationEngineWrap::LoadProfile)});
  }

  LiquidationEngineWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<LiquidationEngineWrap>(info),
        _h(flox_liquidation_engine_create())
  {
  }
  ~LiquidationEngineWrap()
  {
    if (_h)
    {
      flox_liquidation_engine_destroy(_h);
    }
  }
  FloxLiquidationEngineHandle handle() const { return _h; }

 private:
  void AddTier(const Napi::CallbackInfo& info)
  {
    flox_liquidation_engine_add_tier(_h, info[0].As<Napi::Number>().DoubleValue(),
                                     info[1].As<Napi::Number>().DoubleValue());
  }
  void SetInsuranceFundCapital(const Napi::CallbackInfo& info)
  {
    flox_liquidation_engine_set_insurance_fund_capital(
        _h, info[0].As<Napi::Number>().DoubleValue());
  }
  Napi::Value InsuranceFundBalance(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_liquidation_engine_insurance_fund_balance(_h));
  }
  void SetAdlEnabled(const Napi::CallbackInfo& info)
  {
    flox_liquidation_engine_set_adl_enabled(
        _h, info[0].As<Napi::Boolean>().Value() ? 1 : 0);
  }
  void SetLiquidationSlippageBps(const Napi::CallbackInfo& info)
  {
    flox_liquidation_engine_set_liquidation_slippage_bps(
        _h, info[0].As<Napi::Number>().DoubleValue());
  }
  void OpenPosition(const Napi::CallbackInfo& info)
  {
    flox_liquidation_engine_open_position(
        _h, info[0].As<Napi::Number>().Int64Value(),
        info[1].As<Napi::Number>().Uint32Value(),
        info[2].As<Napi::Number>().DoubleValue(),
        info[3].As<Napi::Number>().DoubleValue(),
        info[4].As<Napi::Number>().DoubleValue());
  }
  void ClosePosition(const Napi::CallbackInfo& info)
  {
    flox_liquidation_engine_close_position(
        _h, info[0].As<Napi::Number>().Int64Value(),
        info[1].As<Napi::Number>().Uint32Value());
  }
  Napi::Value OnMark(const Napi::CallbackInfo& info)
  {
    const uint32_t n = flox_liquidation_engine_on_mark(
        _h, info[0].As<Napi::Number>().Uint32Value(),
        info[1].As<Napi::Number>().DoubleValue());
    return Napi::Number::New(info.Env(), n);
  }
  Napi::Value LiquidationsCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(flox_liquidation_engine_liquidations_count(_h)));
  }
  Napi::Value InsurancePaymentsCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(flox_liquidation_engine_insurance_payments_count(_h)));
  }
  Napi::Value AdlCloseoutsCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(flox_liquidation_engine_adl_closeouts_count(_h)));
  }
  void LoadProfile(const Napi::CallbackInfo& info)
  {
    const std::string name = info[0].As<Napi::String>().Utf8Value();
    uint8_t p = 0;
    if (name == "bybit_linear")
    {
      p = 1;
    }
    else if (name == "okx_swap")
    {
      p = 2;
    }
    flox_liquidation_engine_load_profile(_h, p);
  }

  FloxLiquidationEngineHandle _h;
};

inline void registerLiquidationEngine(Napi::Env env, Napi::Object exports)
{
  exports.Set("LiquidationEngine", LiquidationEngineWrap::Init(env));
}

}  // namespace node_flox
