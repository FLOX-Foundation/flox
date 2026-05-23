// node/src/venue_stack.h — single-call venue-realistic backtest wrap.
//
// Exposes a flat proxy surface over the C ABI VenueStack. JS callers
// drive the stack through proxy methods instead of obtaining
// non-owning subsystem wraps; this keeps the existing wrap classes
// (SimulatedExecutor, Account, etc.) ownership-only without a
// repository-wide refactor.

#pragma once

#include <napi.h>

#include <string>

#include "flox/capi/flox_capi.h"

namespace node_flox
{

class VenueStackWrap : public Napi::ObjectWrap<VenueStackWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "VenueStack",
        {InstanceMethod("venueName", &VenueStackWrap::VenueName),
         // Account proxies.
         InstanceMethod("accountId", &VenueStackWrap::AccountId),
         InstanceMethod("accountEquity", &VenueStackWrap::AccountEquity),
         InstanceMethod("accountSetEquity", &VenueStackWrap::AccountSetEquity),
         InstanceMethod("accountAddEquity", &VenueStackWrap::AccountAddEquity),
         InstanceMethod("accountOpenPosition",
                        &VenueStackWrap::AccountOpenPosition),
         InstanceMethod("accountClosePosition",
                        &VenueStackWrap::AccountClosePosition),
         InstanceMethod("accountPositionCount",
                        &VenueStackWrap::AccountPositionCount),
         InstanceMethod("accountSetMark", &VenueStackWrap::AccountSetMark),
         InstanceMethod("accountTotalNotional",
                        &VenueStackWrap::AccountTotalNotional),
         InstanceMethod("accountTotalUnrealisedPnl",
                        &VenueStackWrap::AccountTotalUnrealisedPnl),
         InstanceMethod("accountRollingNotional30d",
                        &VenueStackWrap::AccountRollingNotional30d),
         // Liquidation proxies.
         InstanceMethod("liquidationOnMark", &VenueStackWrap::LiquidationOnMark),
         InstanceMethod("liquidationsCount",
                        &VenueStackWrap::LiquidationsCount),
         InstanceMethod("insuranceFundBalance",
                        &VenueStackWrap::InsuranceFundBalance),
         // Fees proxies.
         InstanceMethod("feesRecordFill", &VenueStackWrap::FeesRecordFill),
         InstanceMethod("feesCurrentTierIndex",
                        &VenueStackWrap::FeesCurrentTierIndex),
         InstanceMethod("feesFor", &VenueStackWrap::FeesFor),
         // Static factories.
         StaticMethod("binanceUmFutures", &VenueStackWrap::BinanceUmFutures),
         StaticMethod("bybitLinear", &VenueStackWrap::BybitLinear),
         StaticMethod("okxSwap", &VenueStackWrap::OkxSwap),
         StaticMethod("deribit", &VenueStackWrap::Deribit),
         StaticMethod("fromVenue", &VenueStackWrap::FromVenue)});
  }

  VenueStackWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<VenueStackWrap>(info)
  {
    const uint8_t venueCode =
        info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
    const uint64_t accountId =
        info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 0;
    const double equity =
        info.Length() > 2 ? info[2].As<Napi::Number>().DoubleValue() : 0.0;
    _h = flox_venue_stack_create(venueCode, accountId, equity);
  }
  ~VenueStackWrap()
  {
    if (_h)
    {
      flox_venue_stack_destroy(_h);
    }
  }

 private:
  static Napi::Value buildFromVenue(const Napi::CallbackInfo& info,
                                    uint8_t venue)
  {
    Napi::Function ctor = info.Env()
                              .Global()
                              .Get("Object")
                              .As<Napi::Object>()
                              .Get("VenueStack")
                              .As<Napi::Function>();
    // The constructor is exported on the module, not on global.Object.
    // Look it up via the caller's `this` binding (the module).
    if (!ctor.IsFunction())
    {
      ctor =
          info.This().As<Napi::Object>().Get("constructor").As<Napi::Function>();
    }
    return ctor.New(
        {Napi::Number::New(info.Env(), venue), info[0], info[1]});
  }
  static Napi::Value BinanceUmFutures(const Napi::CallbackInfo& info)
  {
    return buildFromVenue(info, 0);
  }
  static Napi::Value BybitLinear(const Napi::CallbackInfo& info)
  {
    return buildFromVenue(info, 1);
  }
  static Napi::Value OkxSwap(const Napi::CallbackInfo& info)
  {
    return buildFromVenue(info, 2);
  }
  static Napi::Value Deribit(const Napi::CallbackInfo& info)
  {
    return buildFromVenue(info, 3);
  }
  static Napi::Value FromVenue(const Napi::CallbackInfo& info)
  {
    const std::string name =
        info[0].IsString() ? info[0].As<Napi::String>().Utf8Value() : "";
    uint8_t code = 0;
    if (name == "bybit_linear" || name == "bybit")
    {
      code = 1;
    }
    else if (name == "okx_swap" || name == "okx")
    {
      code = 2;
    }
    else if (name == "deribit")
    {
      code = 3;
    }
    else if (name == "binance_um_futures" || name == "binance")
    {
      code = 0;
    }
    else
    {
      Napi::Error::New(info.Env(), "unknown venue: " + name)
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    Napi::Function ctor =
        info.This().As<Napi::Object>().Get("constructor").As<Napi::Function>();
    return ctor.New({Napi::Number::New(info.Env(), code), info[1], info[2]});
  }

  Napi::Value VenueName(const Napi::CallbackInfo& info)
  {
    const char* name = flox_venue_stack_venue_name(_h);
    return Napi::String::New(info.Env(), name ? name : "");
  }
  FloxAccountHandle acct() { return flox_venue_stack_account(_h); }
  FloxLiquidationEngineHandle liq() { return flox_venue_stack_liquidation(_h); }
  FloxFeeScheduleHandle fees() { return flox_venue_stack_fees(_h); }

  Napi::Value AccountId(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(flox_account_id(acct())));
  }
  Napi::Value AccountEquity(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_account_equity(acct()));
  }
  void AccountSetEquity(const Napi::CallbackInfo& info)
  {
    flox_account_set_equity(acct(), info[0].As<Napi::Number>().DoubleValue());
  }
  void AccountAddEquity(const Napi::CallbackInfo& info)
  {
    flox_account_add_equity(acct(), info[0].As<Napi::Number>().DoubleValue());
  }
  void AccountOpenPosition(const Napi::CallbackInfo& info)
  {
    flox_account_open_position(acct(), info[0].As<Napi::Number>().Uint32Value(),
                               info[1].As<Napi::Number>().DoubleValue(),
                               info[2].As<Napi::Number>().DoubleValue());
  }
  void AccountClosePosition(const Napi::CallbackInfo& info)
  {
    flox_account_close_position(acct(),
                                info[0].As<Napi::Number>().Uint32Value());
  }
  Napi::Value AccountPositionCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_account_position_count(acct()));
  }
  void AccountSetMark(const Napi::CallbackInfo& info)
  {
    flox_account_set_mark(acct(), info[0].As<Napi::Number>().Uint32Value(),
                          info[1].As<Napi::Number>().DoubleValue());
  }
  Napi::Value AccountTotalNotional(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_account_total_notional(acct()));
  }
  Napi::Value AccountTotalUnrealisedPnl(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             flox_account_total_unrealised_pnl(acct()));
  }
  Napi::Value AccountRollingNotional30d(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             flox_account_rolling_notional_30d(acct()));
  }
  Napi::Value LiquidationOnMark(const Napi::CallbackInfo& info)
  {
    const uint32_t n = flox_liquidation_engine_on_mark(
        liq(), info[0].As<Napi::Number>().Uint32Value(),
        info[1].As<Napi::Number>().DoubleValue());
    return Napi::Number::New(info.Env(), n);
  }
  Napi::Value LiquidationsCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(),
        static_cast<double>(flox_liquidation_engine_liquidations_count(liq())));
  }
  Napi::Value InsuranceFundBalance(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(), flox_liquidation_engine_insurance_fund_balance(liq()));
  }
  void FeesRecordFill(const Napi::CallbackInfo& info)
  {
    flox_fee_schedule_record_fill(fees(), info[0].As<Napi::Number>().Int64Value(),
                                  info[1].As<Napi::Number>().DoubleValue());
  }
  Napi::Value FeesCurrentTierIndex(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_fee_schedule_current_tier(fees()));
  }
  Napi::Value FeesFor(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(),
        flox_fee_schedule_fee_for(fees(),
                                  info[0].As<Napi::Number>().Int64Value(),
                                  info[1].As<Napi::Number>().DoubleValue(),
                                  info[2].As<Napi::Boolean>().Value() ? 1 : 0));
  }

  FloxVenueStackHandle _h{nullptr};
};

inline void registerVenueStack(Napi::Env env, Napi::Object exports)
{
  exports.Set("VenueStack", VenueStackWrap::Init(env));
}

}  // namespace node_flox
