// node/src/order_group.h — Multi-leg order group state machine wrap.
//
// Thin NAPI wrap over the C ABI flox_order_group_* surface. The group
// is a passive state machine — bindings record submit / fill / cancel
// events and read back state + recommended actions; no I/O.

#pragma once

#include <napi.h>

#include "flox/capi/flox_capi.h"

namespace node_flox
{

class OrderGroupWrap : public Napi::ObjectWrap<OrderGroupWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "OrderGroup",
                       {InstanceMethod("addMarketLeg", &OrderGroupWrap::AddMarketLeg),
                        InstanceMethod("addLimitLeg", &OrderGroupWrap::AddLimitLeg),
                        InstanceMethod("legCount", &OrderGroupWrap::LegCount),
                        InstanceMethod("legState", &OrderGroupWrap::LegState),
                        InstanceMethod("legFilled", &OrderGroupWrap::LegFilled),
                        InstanceMethod("legOrderId", &OrderGroupWrap::LegOrderId),
                        InstanceMethod("recordSubmit", &OrderGroupWrap::RecordSubmit),
                        InstanceMethod("recordFill", &OrderGroupWrap::RecordFill),
                        InstanceMethod("recordCancel", &OrderGroupWrap::RecordCancel),
                        InstanceMethod("recordFailure", &OrderGroupWrap::RecordFailure),
                        InstanceMethod("state", &OrderGroupWrap::State),
                        InstanceMethod("recommendedActions",
                                       &OrderGroupWrap::RecommendedActions),
                        InstanceMethod("markActionDispatched",
                                       &OrderGroupWrap::MarkActionDispatched),
                        InstanceMethod("autoDispatch", &OrderGroupWrap::AutoDispatch)});
  }

  OrderGroupWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<OrderGroupWrap>(info)
  {
    uint64_t parent = 0;
    uint8_t policy = 0;
    if (info.Length() > 0 && info[0].IsObject())
    {
      auto opts = info[0].As<Napi::Object>();
      if (opts.Has("parentSignalId"))
      {
        parent =
            static_cast<uint64_t>(opts.Get("parentSignalId").As<Napi::Number>().Int64Value());
      }
      if (opts.Has("policy"))
      {
        auto p = opts.Get("policy");
        if (p.IsString())
        {
          std::string s = p.As<Napi::String>().Utf8Value();
          if (s == "BestEffort")
          {
            policy = 0;
          }
          else if (s == "AllOrNothing")
          {
            policy = 1;
          }
          else if (s == "OneSided")
          {
            policy = 2;
          }
          else
          {
            Napi::TypeError::New(info.Env(),
                                 "Unknown OrderGroup policy: '" + s +
                                     "'. Use 'BestEffort' / 'AllOrNothing' / 'OneSided'.")
                .ThrowAsJavaScriptException();
            return;
          }
        }
        else
        {
          policy = static_cast<uint8_t>(p.As<Napi::Number>().Uint32Value());
        }
      }
    }
    _h = flox_order_group_create(parent, policy);
  }

  ~OrderGroupWrap()
  {
    if (_h)
    {
      flox_order_group_destroy(_h);
      _h = nullptr;
    }
  }

 private:
  FloxOrderGroupHandle _h{nullptr};

  Napi::Value AddMarketLeg(const Napi::CallbackInfo& info)
  {
    uint32_t sym = info[0].As<Napi::Number>().Uint32Value();
    uint8_t side = static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value());
    double qty = info[2].As<Napi::Number>().DoubleValue();
    int64_t qty_raw = flox_quantity_from_double(qty);
    return Napi::Number::New(info.Env(),
                             flox_order_group_add_market_leg(_h, sym, side, qty_raw));
  }

  Napi::Value AddLimitLeg(const Napi::CallbackInfo& info)
  {
    uint32_t sym = info[0].As<Napi::Number>().Uint32Value();
    uint8_t side = static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value());
    double price = info[2].As<Napi::Number>().DoubleValue();
    double qty = info[3].As<Napi::Number>().DoubleValue();
    return Napi::Number::New(info.Env(), flox_order_group_add_limit_leg(
                                             _h, sym, side, flox_price_from_double(price),
                                             flox_quantity_from_double(qty)));
  }

  Napi::Value LegCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_order_group_leg_count(_h));
  }

  Napi::Value LegState(const Napi::CallbackInfo& info)
  {
    uint32_t idx = info[0].As<Napi::Number>().Uint32Value();
    static const char* const names[] = {
        "Pending", "Submitted", "PartiallyFilled", "Filled", "Cancelled", "Failed"};
    uint8_t s = flox_order_group_leg_state(_h, idx);
    const char* name = (s < 6) ? names[s] : "Unknown";
    return Napi::String::New(info.Env(), name);
  }

  Napi::Value LegFilled(const Napi::CallbackInfo& info)
  {
    uint32_t idx = info[0].As<Napi::Number>().Uint32Value();
    return Napi::Number::New(
        info.Env(), flox_quantity_to_double(flox_order_group_leg_filled_raw(_h, idx)));
  }

  Napi::Value LegOrderId(const Napi::CallbackInfo& info)
  {
    uint32_t idx = info[0].As<Napi::Number>().Uint32Value();
    return Napi::Number::New(
        info.Env(), static_cast<double>(flox_order_group_leg_order_id(_h, idx)));
  }

  void RecordSubmit(const Napi::CallbackInfo& info)
  {
    uint32_t idx = info[0].As<Napi::Number>().Uint32Value();
    uint64_t order_id =
        static_cast<uint64_t>(info[1].As<Napi::Number>().Int64Value());
    flox_order_group_record_submit(_h, idx, order_id);
  }

  void RecordFill(const Napi::CallbackInfo& info)
  {
    uint32_t idx = info[0].As<Napi::Number>().Uint32Value();
    double cum = info[1].As<Napi::Number>().DoubleValue();
    flox_order_group_record_fill(_h, idx, flox_quantity_from_double(cum));
  }

  void RecordCancel(const Napi::CallbackInfo& info)
  {
    flox_order_group_record_cancel(_h, info[0].As<Napi::Number>().Uint32Value());
  }

  void RecordFailure(const Napi::CallbackInfo& info)
  {
    flox_order_group_record_failure(_h, info[0].As<Napi::Number>().Uint32Value());
  }

  Napi::Value State(const Napi::CallbackInfo& info)
  {
    static const char* const names[] = {
        "Pending", "Submitted", "PartiallyFilled", "Filled",
        "Cancelled", "Reverting", "Failed"};
    uint8_t s = flox_order_group_state(_h);
    const char* name = (s < 7) ? names[s] : "Unknown";
    return Napi::String::New(info.Env(), name);
  }

  Napi::Value RecommendedActions(const Napi::CallbackInfo& info)
  {
    constexpr uint32_t kMaxActions = 32;
    int64_t buf[kMaxActions * 5];
    uint32_t n = flox_order_group_recommended_actions(_h, buf, kMaxActions);
    Napi::Array out = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      const int64_t* slot = buf + i * 5;
      Napi::Object obj = Napi::Object::New(info.Env());
      obj.Set("kind", Napi::String::New(info.Env(), slot[0] == 0 ? "cancel" : "revert"));
      obj.Set("legIndex", Napi::Number::New(info.Env(), static_cast<double>(slot[1])));
      if (slot[0] == 0)
      {
        obj.Set("orderId", Napi::Number::New(info.Env(), static_cast<double>(slot[2])));
      }
      else
      {
        obj.Set("symbol", Napi::Number::New(info.Env(), static_cast<double>(slot[2])));
        obj.Set("side", Napi::Number::New(info.Env(), static_cast<double>(slot[3])));
        obj.Set("qty", Napi::Number::New(info.Env(), flox_quantity_to_double(slot[4])));
      }
      out.Set(i, obj);
    }
    return out;
  }

  void MarkActionDispatched(const Napi::CallbackInfo& info)
  {
    uint32_t leg = info[0].As<Napi::Number>().Uint32Value();
    std::string kind = info[1].As<Napi::String>().Utf8Value();
    uint8_t k = (kind == "cancel") ? 0 : 1;
    flox_order_group_mark_action_dispatched(_h, leg, k);
  }

  Napi::Value AutoDispatch(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || !info[0].IsObject())
    {
      Napi::TypeError::New(env, "autoDispatch requires a strategy object")
          .ThrowAsJavaScriptException();
      return Napi::Number::New(env, 0);
    }
    auto strategy = info[0].As<Napi::Object>();

    constexpr uint32_t kMaxActions = 32;
    int64_t buf[kMaxActions * 5];
    uint32_t n = flox_order_group_recommended_actions(_h, buf, kMaxActions);
    uint32_t fired = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
      const int64_t* slot = buf + i * 5;
      uint32_t leg = static_cast<uint32_t>(slot[1]);
      if (slot[0] == 0)
      {
        // CancelLeg: strategy.emitCancel(orderId).
        Napi::Function fn = strategy.Get("emitCancel").As<Napi::Function>();
        fn.Call(strategy, {Napi::Number::New(env, static_cast<double>(slot[2]))});
        flox_order_group_mark_action_dispatched(_h, leg, 0);
      }
      else
      {
        // RevertLeg: opposite side market order.
        const char* method = (slot[3] == 0) ? "emitMarketBuy" : "emitMarketSell";
        Napi::Function fn = strategy.Get(method).As<Napi::Function>();
        fn.Call(strategy, {Napi::Number::New(env, static_cast<double>(slot[2])),
                           Napi::Number::New(env, flox_quantity_to_double(slot[4]))});
        flox_order_group_mark_action_dispatched(_h, leg, 1);
      }
      ++fired;
    }
    return Napi::Number::New(env, fired);
  }
};

inline void registerOrderGroup(Napi::Env env, Napi::Object exports)
{
  exports.Set("OrderGroup", OrderGroupWrap::Init(env));

  // Named constants so users can write
  // `new OrderGroup({ policy: flox.OrderGroupPolicy.AllOrNothing })`.
  Napi::Object policy = Napi::Object::New(env);
  policy.Set("BestEffort", Napi::String::New(env, "BestEffort"));
  policy.Set("AllOrNothing", Napi::String::New(env, "AllOrNothing"));
  policy.Set("OneSided", Napi::String::New(env, "OneSided"));
  policy.Freeze();
  exports.Set("OrderGroupPolicy", policy);

  Napi::Object groupState = Napi::Object::New(env);
  groupState.Set("Pending", Napi::String::New(env, "Pending"));
  groupState.Set("Submitted", Napi::String::New(env, "Submitted"));
  groupState.Set("PartiallyFilled", Napi::String::New(env, "PartiallyFilled"));
  groupState.Set("Filled", Napi::String::New(env, "Filled"));
  groupState.Set("Cancelled", Napi::String::New(env, "Cancelled"));
  groupState.Set("Reverting", Napi::String::New(env, "Reverting"));
  groupState.Set("Failed", Napi::String::New(env, "Failed"));
  groupState.Freeze();
  exports.Set("OrderGroupState", groupState);
}

}  // namespace node_flox
