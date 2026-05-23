// node/src/fee_schedule.h — Tiered maker/taker fee ladder.

#pragma once

#include <napi.h>

#include <vector>

#include "account.h"
#include "flox/capi/flox_capi.h"

namespace node_flox
{

class FeeScheduleWrap : public Napi::ObjectWrap<FeeScheduleWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "FeeSchedule",
        {InstanceMethod("addTier", &FeeScheduleWrap::AddTier),
         InstanceMethod("loadProfile", &FeeScheduleWrap::LoadProfile),
         InstanceMethod("recordFill", &FeeScheduleWrap::RecordFill),
         InstanceMethod("feeFor", &FeeScheduleWrap::FeeFor),
         InstanceMethod("currentTierIndex", &FeeScheduleWrap::CurrentTier),
         InstanceMethod("rollingNotional30d", &FeeScheduleWrap::RollingNotional),
         InstanceMethod("tierTransitions", &FeeScheduleWrap::TierTransitions),
         InstanceMethod("resetRolling", &FeeScheduleWrap::ResetRolling),
         InstanceMethod("bindAccount", &FeeScheduleWrap::BindAccount),
         InstanceMethod("clearAccountBinding", &FeeScheduleWrap::ClearAccountBinding)});
  }

  FeeScheduleWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<FeeScheduleWrap>(info), _h(flox_fee_schedule_create())
  {
  }
  ~FeeScheduleWrap()
  {
    if (_h)
    {
      flox_fee_schedule_destroy(_h);
    }
  }
  FloxFeeScheduleHandle handle() const { return _h; }

 private:
  void AddTier(const Napi::CallbackInfo& info)
  {
    flox_fee_schedule_add_tier(_h, info[0].As<Napi::Number>().DoubleValue(),
                               info[1].As<Napi::Number>().DoubleValue(),
                               info[2].As<Napi::Number>().DoubleValue());
  }
  void LoadProfile(const Napi::CallbackInfo& info)
  {
    std::string name = info[0].As<Napi::String>().Utf8Value();
    flox_fee_schedule_load_profile(_h, name.c_str());
  }
  void RecordFill(const Napi::CallbackInfo& info)
  {
    flox_fee_schedule_record_fill(_h, info[0].As<Napi::Number>().Int64Value(),
                                  info[1].As<Napi::Number>().DoubleValue());
  }
  Napi::Value FeeFor(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(), flox_fee_schedule_fee_for(
                        _h, info[0].As<Napi::Number>().Int64Value(),
                        info[1].As<Napi::Number>().DoubleValue(),
                        info[2].As<Napi::Boolean>().Value() ? 1 : 0));
  }
  Napi::Value CurrentTier(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_fee_schedule_current_tier(_h));
  }
  Napi::Value RollingNotional(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_fee_schedule_rolling_notional(_h));
  }
  Napi::Value TierTransitions(const Napi::CallbackInfo& info)
  {
    uint32_t n = flox_fee_schedule_tier_transitions(_h, nullptr, 0);
    std::vector<int64_t> buf(n, 0);
    flox_fee_schedule_tier_transitions(_h, buf.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      arr.Set(i, Napi::Number::New(info.Env(), static_cast<double>(buf[i])));
    }
    return arr;
  }
  void ResetRolling(const Napi::CallbackInfo&) { flox_fee_schedule_reset_rolling(_h); }
  void BindAccount(const Napi::CallbackInfo& info)
  {
    if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined())
    {
      flox_fee_schedule_bind_account(_h, nullptr);
      return;
    }
    auto* w = Napi::ObjectWrap<AccountWrap>::Unwrap(info[0].As<Napi::Object>());
    flox_fee_schedule_bind_account(_h, w->handle());
  }
  void ClearAccountBinding(const Napi::CallbackInfo&)
  {
    flox_fee_schedule_clear_account_binding(_h);
  }

  FloxFeeScheduleHandle _h;
};

inline void registerFeeSchedule(Napi::Env env, Napi::Object exports)
{
  exports.Set("FeeSchedule", FeeScheduleWrap::Init(env));
}

}  // namespace node_flox
