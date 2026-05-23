// node/src/liquidation_engine.h — Liquidation + insurance + ADL wrap.

#pragma once

#include <napi.h>

#include <string>

#include "backtest.h"
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
         InstanceMethod("setAdlRanking", &LiquidationEngineWrap::SetAdlRanking),
         InstanceMethod("adlRanking", &LiquidationEngineWrap::AdlRanking),
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
         InstanceMethod("loadProfile", &LiquidationEngineWrap::LoadProfile),
         InstanceMethod("setExecutor", &LiquidationEngineWrap::SetExecutor),
         InstanceMethod("deficitsPaidByFund",
                        &LiquidationEngineWrap::DeficitsPaidByFund),
         InstanceMethod("deficitsPaidByAdl",
                        &LiquidationEngineWrap::DeficitsPaidByAdl),
         InstanceMethod("cascadeSizesPerTick",
                        &LiquidationEngineWrap::CascadeSizesPerTick),
         InstanceMethod("fundBalanceHistory",
                        &LiquidationEngineWrap::FundBalanceHistory),
         InstanceMethod("ticksToFirstAdl",
                        &LiquidationEngineWrap::TicksToFirstAdl),
         InstanceMethod("resetStats", &LiquidationEngineWrap::ResetStats)});
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
  void SetAdlRanking(const Napi::CallbackInfo& info)
  {
    uint8_t code = 0;
    if (info.Length() >= 1 && info[0].IsString())
    {
      const std::string name = info[0].As<Napi::String>().Utf8Value();
      if (name == "binance")
      {
        code = 1;
      }
      else if (name == "bybit")
      {
        code = 2;
      }
      else if (name == "position_size")
      {
        code = 3;
      }
    }
    else if (info.Length() >= 1 && info[0].IsNumber())
    {
      code = static_cast<uint8_t>(info[0].As<Napi::Number>().Uint32Value());
    }
    flox_liquidation_engine_set_adl_ranking(_h, code);
  }
  Napi::Value AdlRanking(const Napi::CallbackInfo& info)
  {
    const uint8_t code = flox_liquidation_engine_adl_ranking(_h);
    const char* name = "pnl_ratio";
    switch (code)
    {
      case 1:
        name = "binance";
        break;
      case 2:
        name = "bybit";
        break;
      case 3:
        name = "position_size";
        break;
      default:
        name = "pnl_ratio";
        break;
    }
    return Napi::String::New(info.Env(), name);
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
  void SetExecutor(const Napi::CallbackInfo& info)
  {
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      flox_liquidation_engine_set_executor(_h, nullptr);
      return;
    }
    auto* w = Napi::ObjectWrap<SimulatedExecutorWrap>::Unwrap(
        info[0].As<Napi::Object>());
    flox_liquidation_engine_set_executor(_h, w->handle());
  }
  Napi::Value DeficitsPaidByFund(const Napi::CallbackInfo& info)
  {
    const uint32_t n = flox_liquidation_engine_deficits_paid_by_fund_size(_h);
    std::vector<double> buf(n);
    flox_liquidation_engine_deficits_paid_by_fund_copy(_h, buf.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      arr.Set(i, Napi::Number::New(info.Env(), buf[i]));
    }
    return arr;
  }
  Napi::Value DeficitsPaidByAdl(const Napi::CallbackInfo& info)
  {
    const uint32_t n = flox_liquidation_engine_deficits_paid_by_adl_size(_h);
    std::vector<double> buf(n);
    flox_liquidation_engine_deficits_paid_by_adl_copy(_h, buf.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      arr.Set(i, Napi::Number::New(info.Env(), buf[i]));
    }
    return arr;
  }
  Napi::Value CascadeSizesPerTick(const Napi::CallbackInfo& info)
  {
    const uint32_t n = flox_liquidation_engine_cascade_sizes_size(_h);
    std::vector<uint32_t> buf(n);
    flox_liquidation_engine_cascade_sizes_copy(_h, buf.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      arr.Set(i, Napi::Number::New(info.Env(), buf[i]));
    }
    return arr;
  }
  Napi::Value FundBalanceHistory(const Napi::CallbackInfo& info)
  {
    const uint32_t n = flox_liquidation_engine_fund_balance_history_size(_h);
    std::vector<double> buf(n);
    flox_liquidation_engine_fund_balance_history_copy(_h, buf.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      arr.Set(i, Napi::Number::New(info.Env(), buf[i]));
    }
    return arr;
  }
  Napi::Value TicksToFirstAdl(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(),
        static_cast<double>(flox_liquidation_engine_ticks_to_first_adl(_h)));
  }
  void ResetStats(const Napi::CallbackInfo& info)
  {
    flox_liquidation_engine_reset_stats(_h);
  }

  FloxLiquidationEngineHandle _h;
};

inline void registerLiquidationEngine(Napi::Env env, Napi::Object exports)
{
  exports.Set("LiquidationEngine", LiquidationEngineWrap::Init(env));
}

}  // namespace node_flox
