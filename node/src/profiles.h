// node/src/profiles.h -- VolumeProfile, MarketProfile, FootprintBar, CompositeBookMatrix

#pragma once
#include <napi.h>
#include "flox/capi/flox_capi.h"

namespace node_flox
{

class VolumeProfileWrap : public Napi::ObjectWrap<VolumeProfileWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "VolumeProfile",
      {InstanceMethod("addTrade", &VolumeProfileWrap::AddTrade),
       InstanceMethod("poc", &VolumeProfileWrap::Poc),
       InstanceMethod("valueAreaHigh", &VolumeProfileWrap::Vah),
       InstanceMethod("valueAreaLow", &VolumeProfileWrap::Val),
       InstanceMethod("totalVolume", &VolumeProfileWrap::TotalVol),
       InstanceMethod("clear", &VolumeProfileWrap::Clear)});
  }
  VolumeProfileWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<VolumeProfileWrap>(info),
    _h(flox_volume_profile_create(info[0].As<Napi::Number>().DoubleValue())) {}
  ~VolumeProfileWrap() { if (_h) flox_volume_profile_destroy(_h); }
 private:
  void AddTrade(const Napi::CallbackInfo& info) { flox_volume_profile_add_trade(_h, info[0].As<Napi::Number>().DoubleValue(), info[1].As<Napi::Number>().DoubleValue(), info[2].As<Napi::Boolean>().Value() ? 1 : 0); }
  Napi::Value Poc(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_volume_profile_poc(_h)); }
  Napi::Value Vah(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_volume_profile_vah(_h)); }
  Napi::Value Val(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_volume_profile_val(_h)); }
  Napi::Value TotalVol(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_volume_profile_total_volume(_h)); }
  void Clear(const Napi::CallbackInfo&) { flox_volume_profile_clear(_h); }
  FloxVolumeProfileHandle _h;
};

class MarketProfileWrap : public Napi::ObjectWrap<MarketProfileWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "MarketProfile",
      {InstanceMethod("addTrade", &MarketProfileWrap::AddTrade),
       InstanceMethod("poc", &MarketProfileWrap::Poc),
       InstanceMethod("valueAreaHigh", &MarketProfileWrap::Vah),
       InstanceMethod("valueAreaLow", &MarketProfileWrap::Val),
       InstanceMethod("initialBalanceHigh", &MarketProfileWrap::IbHigh),
       InstanceMethod("initialBalanceLow", &MarketProfileWrap::IbLow),
       InstanceMethod("isPoorHigh", &MarketProfileWrap::PoorHigh),
       InstanceMethod("isPoorLow", &MarketProfileWrap::PoorLow),
       InstanceMethod("clear", &MarketProfileWrap::Clear)});
  }
  MarketProfileWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<MarketProfileWrap>(info),
    _h(flox_market_profile_create(info[0].As<Napi::Number>().DoubleValue(),
      info[1].As<Napi::Number>().Uint32Value(), info[2].As<Napi::Number>().Int64Value())) {}
  ~MarketProfileWrap() { if (_h) flox_market_profile_destroy(_h); }
 private:
  void AddTrade(const Napi::CallbackInfo& info) { flox_market_profile_add_trade(_h, info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().DoubleValue(), info[2].As<Napi::Number>().DoubleValue(), info[3].As<Napi::Boolean>().Value() ? 1 : 0); }
  Napi::Value Poc(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_market_profile_poc(_h)); }
  Napi::Value Vah(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_market_profile_vah(_h)); }
  Napi::Value Val(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_market_profile_val(_h)); }
  Napi::Value IbHigh(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_market_profile_ib_high(_h)); }
  Napi::Value IbLow(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_market_profile_ib_low(_h)); }
  Napi::Value PoorHigh(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), flox_market_profile_is_poor_high(_h)); }
  Napi::Value PoorLow(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), flox_market_profile_is_poor_low(_h)); }
  void Clear(const Napi::CallbackInfo&) { flox_market_profile_clear(_h); }
  FloxMarketProfileHandle _h;
};

class FootprintBarWrap : public Napi::ObjectWrap<FootprintBarWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "FootprintBar",
      {InstanceMethod("addTrade", &FootprintBarWrap::AddTrade),
       InstanceMethod("totalDelta", &FootprintBarWrap::Delta),
       InstanceMethod("totalVolume", &FootprintBarWrap::Volume),
       InstanceAccessor("numLevels", &FootprintBarWrap::Levels, nullptr),
       InstanceMethod("clear", &FootprintBarWrap::Clear)});
  }
  FootprintBarWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<FootprintBarWrap>(info),
    _h(flox_footprint_create(info[0].As<Napi::Number>().DoubleValue())) {}
  ~FootprintBarWrap() { if (_h) flox_footprint_destroy(_h); }
 private:
  void AddTrade(const Napi::CallbackInfo& info) { flox_footprint_add_trade(_h, info[0].As<Napi::Number>().DoubleValue(), info[1].As<Napi::Number>().DoubleValue(), info[2].As<Napi::Boolean>().Value() ? 1 : 0); }
  Napi::Value Delta(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_footprint_total_delta(_h)); }
  Napi::Value Volume(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_footprint_total_volume(_h)); }
  Napi::Value Levels(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_footprint_num_levels(_h)); }
  void Clear(const Napi::CallbackInfo&) { flox_footprint_clear(_h); }
  FloxFootprintHandle _h;
};

inline void registerProfiles(Napi::Env env, Napi::Object exports)
{
  exports.Set("VolumeProfile", VolumeProfileWrap::Init(env));
  exports.Set("MarketProfile", MarketProfileWrap::Init(env));
  exports.Set("FootprintBar", FootprintBarWrap::Init(env));
}

}  // namespace node_flox
