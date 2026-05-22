// node/src/funding_schedule.h — Perpetual funding rate schedule.

#pragma once

#include <napi.h>

#include <vector>

#include "flox/capi/flox_capi.h"

namespace node_flox
{

class FundingScheduleWrap : public Napi::ObjectWrap<FundingScheduleWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "FundingSchedule",
        {InstanceMethod("setConstant", &FundingScheduleWrap::SetConstant),
         InstanceMethod("setTape", &FundingScheduleWrap::SetTape),
         InstanceMethod("loadProfile", &FundingScheduleWrap::LoadProfile),
         InstanceMethod("setConstantRate", &FundingScheduleWrap::SetConstantRate),
         InstanceMethod("reset", &FundingScheduleWrap::Reset),
         InstanceMethod("tick", &FundingScheduleWrap::Tick)});
  }

  FundingScheduleWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<FundingScheduleWrap>(info),
        _h(flox_funding_schedule_create())
  {
  }
  ~FundingScheduleWrap()
  {
    if (_h)
    {
      flox_funding_schedule_destroy(_h);
    }
  }
  FloxFundingScheduleHandle handle() const { return _h; }

 private:
  void SetConstant(const Napi::CallbackInfo& info)
  {
    flox_funding_schedule_set_constant(_h, info[0].As<Napi::Number>().Int64Value(),
                                       info[1].As<Napi::Number>().DoubleValue());
  }
  void SetTape(const Napi::CallbackInfo& info)
  {
    auto tsArr = info[0].As<Napi::Array>();
    auto rateArr = info[1].As<Napi::Array>();
    std::vector<int64_t> ts(tsArr.Length());
    std::vector<double> rates(rateArr.Length());
    for (uint32_t i = 0; i < tsArr.Length(); ++i)
    {
      ts[i] = tsArr.Get(i).As<Napi::Number>().Int64Value();
    }
    for (uint32_t i = 0; i < rateArr.Length(); ++i)
    {
      rates[i] = rateArr.Get(i).As<Napi::Number>().DoubleValue();
    }
    flox_funding_schedule_set_tape(_h, ts.data(), rates.data(),
                                   static_cast<uint32_t>(ts.size()));
  }
  void LoadProfile(const Napi::CallbackInfo& info)
  {
    std::string name = info[0].As<Napi::String>().Utf8Value();
    flox_funding_schedule_load_profile(_h, name.c_str());
  }
  void SetConstantRate(const Napi::CallbackInfo& info)
  {
    flox_funding_schedule_set_constant_rate(_h, info[0].As<Napi::Number>().DoubleValue());
  }
  void Reset(const Napi::CallbackInfo&) { flox_funding_schedule_reset(_h); }

  Napi::Value Tick(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = info[0].As<Napi::Number>().Int64Value();
    auto symbols = info[1].As<Napi::Array>();
    auto positions = info[2].As<Napi::Array>();
    auto marks = info[3].As<Napi::Array>();
    uint32_t n = symbols.Length();
    std::vector<uint32_t> sy(n);
    std::vector<double> ps(n);
    std::vector<double> mk(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      sy[i] = symbols.Get(i).As<Napi::Number>().Uint32Value();
      ps[i] = positions.Get(i).As<Napi::Number>().DoubleValue();
      mk[i] = marks.Get(i).As<Napi::Number>().DoubleValue();
    }
    uint32_t count = flox_funding_schedule_tick(_h, nowNs, sy.data(), ps.data(),
                                                mk.data(), n, nullptr, 0);
    std::vector<double> buf(count * 6, 0.0);
    flox_funding_schedule_tick(_h, nowNs, sy.data(), ps.data(), mk.data(), n, buf.data(),
                               count);
    Napi::Array out = Napi::Array::New(info.Env(), count);
    for (uint32_t i = 0; i < count; ++i)
    {
      Napi::Object o = Napi::Object::New(info.Env());
      o.Set("timestampNs", Napi::Number::New(info.Env(), buf[i * 6 + 0]));
      o.Set("symbol", Napi::Number::New(info.Env(), buf[i * 6 + 1]));
      o.Set("rate", Napi::Number::New(info.Env(), buf[i * 6 + 2]));
      o.Set("markPrice", Napi::Number::New(info.Env(), buf[i * 6 + 3]));
      o.Set("positionSigned", Napi::Number::New(info.Env(), buf[i * 6 + 4]));
      o.Set("amount", Napi::Number::New(info.Env(), buf[i * 6 + 5]));
      out.Set(i, o);
    }
    return out;
  }

  FloxFundingScheduleHandle _h;
};

inline void registerFundingSchedule(Napi::Env env, Napi::Object exports)
{
  exports.Set("FundingSchedule", FundingScheduleWrap::Init(env));
}

}  // namespace node_flox
