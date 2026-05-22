// node/src/venue_availability.h — VenueAvailability wrap.
//
// Thin NAPI wrap over flox_venue_availability_* C ABI. Build a
// downtime schedule (manual outages + Poisson random outages) and
// attach to a SimulatedExecutor via executor.setVenueAvailability(va).

#pragma once

#include <napi.h>

#include <string>

#include "flox/capi/flox_capi.h"

namespace node_flox
{

inline uint8_t parseOnOutage(const std::string& s)
{
  if (s == "hold")
  {
    return 1;
  }
  if (s == "expire_gtc_after")
  {
    return 2;
  }
  return 0;  // cancel_all
}

class VenueAvailabilityWrap : public Napi::ObjectWrap<VenueAvailabilityWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "VenueAvailability",
        {InstanceMethod("scheduleOutage", &VenueAvailabilityWrap::ScheduleOutage),
         InstanceMethod("autoRandomOutages", &VenueAvailabilityWrap::AutoRandomOutages),
         InstanceMethod("isUp", &VenueAvailabilityWrap::IsUp)});
  }

  VenueAvailabilityWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<VenueAvailabilityWrap>(info),
        _h(flox_venue_availability_create())
  {
  }
  ~VenueAvailabilityWrap()
  {
    if (_h)
    {
      flox_venue_availability_destroy(_h);
    }
  }
  FloxVenueAvailabilityHandle handle() const { return _h; }

 private:
  void ScheduleOutage(const Napi::CallbackInfo& info)
  {
    int64_t start = info[0].As<Napi::Number>().Int64Value();
    int64_t duration = info[1].As<Napi::Number>().Int64Value();
    std::string policy = info.Length() > 2 && info[2].IsString()
                             ? info[2].As<Napi::String>().Utf8Value()
                             : "cancel_all";
    int64_t ttl = info.Length() > 3 ? info[3].As<Napi::Number>().Int64Value() : 0;
    flox_venue_availability_schedule_outage(_h, start, duration, parseOnOutage(policy),
                                            ttl);
  }
  void AutoRandomOutages(const Napi::CallbackInfo& info)
  {
    double perDay = info[0].As<Napi::Number>().DoubleValue();
    int64_t meanDur = info[1].As<Napi::Number>().Int64Value();
    std::string policy = info.Length() > 2 && info[2].IsString()
                             ? info[2].As<Napi::String>().Utf8Value()
                             : "cancel_all";
    uint64_t seed = info.Length() > 3 ? info[3].As<Napi::Number>().Int64Value() : 0xC0FFEEULL;
    flox_venue_availability_auto_random_outages(_h, perDay, meanDur,
                                                parseOnOutage(policy), seed);
  }
  Napi::Value IsUp(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = info[0].As<Napi::Number>().Int64Value();
    return Napi::Boolean::New(info.Env(), flox_venue_availability_is_up(_h, nowNs) != 0);
  }

  FloxVenueAvailabilityHandle _h;
};

inline void registerVenueAvailability(Napi::Env env, Napi::Object exports)
{
  exports.Set("VenueAvailability", VenueAvailabilityWrap::Init(env));
}

}  // namespace node_flox
