// node/src/live_queue_position.h — Live queue position estimator.
//
// Thin NAPI wrap over the C ABI flox_live_queue_position_* surface.
// Client-side heuristic: feed it order placements + trades + book
// updates + our fills, read back estimated queue-ahead per resting
// order with a confidence score.

#pragma once

#include <napi.h>

#include <cstring>

#include "flox/capi/flox_capi.h"

namespace node_flox
{

class LiveQueuePositionEstimatorWrap
    : public Napi::ObjectWrap<LiveQueuePositionEstimatorWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "LiveQueuePositionEstimator",
        {InstanceMethod("setConfidenceHalfLifeNs",
                        &LiveQueuePositionEstimatorWrap::SetConfidenceHalfLifeNs),
         InstanceMethod("setShrinkAttributionFactor",
                        &LiveQueuePositionEstimatorWrap::SetShrinkAttributionFactor),
         InstanceMethod("onOrderPlaced",
                        &LiveQueuePositionEstimatorWrap::OnOrderPlaced),
         InstanceMethod("onOrderCancelled",
                        &LiveQueuePositionEstimatorWrap::OnOrderCancelled),
         InstanceMethod("onOrderFilled",
                        &LiveQueuePositionEstimatorWrap::OnOrderFilled),
         InstanceMethod("onTrade", &LiveQueuePositionEstimatorWrap::OnTrade),
         InstanceMethod("onLevelUpdate",
                        &LiveQueuePositionEstimatorWrap::OnLevelUpdate),
         InstanceMethod("snapshot", &LiveQueuePositionEstimatorWrap::Snapshot),
         InstanceMethod("trackedOrderCount",
                        &LiveQueuePositionEstimatorWrap::TrackedOrderCount)});
  }

  LiveQueuePositionEstimatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<LiveQueuePositionEstimatorWrap>(info)
  {
    _h = flox_live_queue_position_create();
  }

  ~LiveQueuePositionEstimatorWrap()
  {
    if (_h)
    {
      flox_live_queue_position_destroy(_h);
      _h = nullptr;
    }
  }

 private:
  FloxLiveQueuePositionHandle _h{nullptr};

  void SetConfidenceHalfLifeNs(const Napi::CallbackInfo& info)
  {
    flox_live_queue_position_set_confidence_half_life_ns(
        _h, info[0].As<Napi::Number>().Int64Value());
  }

  void SetShrinkAttributionFactor(const Napi::CallbackInfo& info)
  {
    flox_live_queue_position_set_shrink_factor(
        _h, info[0].As<Napi::Number>().DoubleValue());
  }

  void OnOrderPlaced(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = info[0].As<Napi::Number>().Uint32Value();
    uint8_t side = static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value());
    double price = info[2].As<Napi::Number>().DoubleValue();
    uint64_t order_id =
        static_cast<uint64_t>(info[3].As<Napi::Number>().Int64Value());
    double order_qty = info[4].As<Napi::Number>().DoubleValue();
    double level_qty = info[5].As<Napi::Number>().DoubleValue();
    int64_t ts_ns = info.Length() > 6 ? info[6].As<Napi::Number>().Int64Value() : 0;
    flox_live_queue_position_on_order_placed(
        _h, symbol, side, flox_price_from_double(price), order_id,
        flox_quantity_from_double(order_qty), flox_quantity_from_double(level_qty),
        ts_ns);
  }

  void OnOrderCancelled(const Napi::CallbackInfo& info)
  {
    uint64_t order_id =
        static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value());
    int64_t ts_ns = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 0;
    flox_live_queue_position_on_order_cancelled(_h, order_id, ts_ns);
  }

  void OnOrderFilled(const Napi::CallbackInfo& info)
  {
    uint64_t order_id =
        static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value());
    double cum = info[1].As<Napi::Number>().DoubleValue();
    int64_t ts_ns = info.Length() > 2 ? info[2].As<Napi::Number>().Int64Value() : 0;
    flox_live_queue_position_on_order_filled(_h, order_id,
                                             flox_quantity_from_double(cum), ts_ns);
  }

  void OnTrade(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = info[0].As<Napi::Number>().Uint32Value();
    double price = info[1].As<Napi::Number>().DoubleValue();
    double qty = info[2].As<Napi::Number>().DoubleValue();
    int64_t ts_ns = info.Length() > 3 ? info[3].As<Napi::Number>().Int64Value() : 0;
    flox_live_queue_position_on_trade(_h, symbol, flox_price_from_double(price),
                                      flox_quantity_from_double(qty), ts_ns);
  }

  void OnLevelUpdate(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = info[0].As<Napi::Number>().Uint32Value();
    uint8_t side = static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value());
    double price = info[2].As<Napi::Number>().DoubleValue();
    double new_qty = info[3].As<Napi::Number>().DoubleValue();
    int64_t ts_ns = info.Length() > 4 ? info[4].As<Napi::Number>().Int64Value() : 0;
    flox_live_queue_position_on_level_update(_h, symbol, side,
                                             flox_price_from_double(price),
                                             flox_quantity_from_double(new_qty), ts_ns);
  }

  Napi::Value Snapshot(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint64_t order_id =
        static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value());
    int64_t now_ns = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 0;
    int64_t slots[5] = {0};
    uint8_t ok = flox_live_queue_position_snapshot(_h, order_id, now_ns, slots);
    if (!ok)
    {
      return env.Null();
    }
    double conf = 0.0;
    std::memcpy(&conf, &slots[4], sizeof(double));
    Napi::Object out = Napi::Object::New(env);
    out.Set("orderId", Napi::Number::New(env, static_cast<double>(slots[0])));
    out.Set("queueAheadEst",
            Napi::Number::New(env, flox_quantity_to_double(slots[1])));
    out.Set("total", Napi::Number::New(env, flox_quantity_to_double(slots[2])));
    out.Set("lastUpdateNs",
            Napi::Number::New(env, static_cast<double>(slots[3])));
    out.Set("confidence", Napi::Number::New(env, conf));
    return out;
  }

  Napi::Value TrackedOrderCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_live_queue_position_tracked_count(_h));
  }
};

inline void registerLiveQueuePositionEstimator(Napi::Env env, Napi::Object exports)
{
  exports.Set("LiveQueuePositionEstimator",
              LiveQueuePositionEstimatorWrap::Init(env));
}

}  // namespace node_flox
