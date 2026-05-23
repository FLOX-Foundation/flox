// node/src/account.h — Cross-margin Account state shared across W15 subsystems.

#pragma once

#include <napi.h>

#include <string>

#include "flox/capi/flox_capi.h"

namespace node_flox
{

class AccountWrap : public Napi::ObjectWrap<AccountWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "Account",
        {InstanceMethod("accountId", &AccountWrap::AccountId),
         InstanceMethod("equity", &AccountWrap::Equity),
         InstanceMethod("setEquity", &AccountWrap::SetEquity),
         InstanceMethod("addEquity", &AccountWrap::AddEquity),
         InstanceMethod("marginMode", &AccountWrap::MarginMode),
         InstanceMethod("setMarginMode", &AccountWrap::SetMarginMode),
         InstanceMethod("openPosition", &AccountWrap::OpenPosition),
         InstanceMethod("closePosition", &AccountWrap::ClosePosition),
         InstanceMethod("positionCount", &AccountWrap::PositionCount),
         InstanceMethod("setMark", &AccountWrap::SetMark),
         InstanceMethod("markTsFor", &AccountWrap::MarkTsFor),
         InstanceMethod("hasStaleMarks", &AccountWrap::HasStaleMarks),
         InstanceMethod("totalNotional", &AccountWrap::TotalNotional),
         InstanceMethod("totalUnrealisedPnl", &AccountWrap::TotalUpnl),
         InstanceMethod("recordFill", &AccountWrap::RecordFill),
         InstanceMethod("rollingNotionalBySymbol30d",
                        &AccountWrap::RollingNotionalBySymbol),
         InstanceMethod("rollingNotional30d", &AccountWrap::RollingNotional),
         InstanceMethod("resetRolling", &AccountWrap::ResetRolling)});
  }

  AccountWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<AccountWrap>(info)
  {
    const uint64_t id = info.Length() > 0 && info[0].IsNumber()
                            ? info[0].As<Napi::Number>().Int64Value()
                            : 0;
    const double eq = info.Length() > 1 && info[1].IsNumber()
                          ? info[1].As<Napi::Number>().DoubleValue()
                          : 0.0;
    _h = flox_account_create(id, eq);
  }
  ~AccountWrap()
  {
    if (_h)
    {
      flox_account_destroy(_h);
    }
  }
  FloxAccountHandle handle() const { return _h; }

 private:
  Napi::Value AccountId(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(flox_account_id(_h)));
  }
  Napi::Value Equity(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_account_equity(_h));
  }
  void SetEquity(const Napi::CallbackInfo& info)
  {
    flox_account_set_equity(_h, info[0].As<Napi::Number>().DoubleValue());
  }
  void AddEquity(const Napi::CallbackInfo& info)
  {
    flox_account_add_equity(_h, info[0].As<Napi::Number>().DoubleValue());
  }
  Napi::Value MarginMode(const Napi::CallbackInfo& info)
  {
    const uint8_t m = flox_account_margin_mode(_h);
    return Napi::String::New(info.Env(), m == 1 ? "isolated" : "cross");
  }
  void SetMarginMode(const Napi::CallbackInfo& info)
  {
    uint8_t code = 0;
    if (info.Length() >= 1 && info[0].IsString())
    {
      const std::string name = info[0].As<Napi::String>().Utf8Value();
      if (name == "isolated")
      {
        code = 1;
      }
    }
    else if (info.Length() >= 1 && info[0].IsNumber())
    {
      code = static_cast<uint8_t>(info[0].As<Napi::Number>().Uint32Value());
    }
    flox_account_set_margin_mode(_h, code);
  }
  void OpenPosition(const Napi::CallbackInfo& info)
  {
    const uint32_t sym = info[0].As<Napi::Number>().Uint32Value();
    const double qty = info[1].As<Napi::Number>().DoubleValue();
    const double entry = info[2].As<Napi::Number>().DoubleValue();
    if (info.Length() >= 4 && info[3].IsNumber())
    {
      flox_account_open_position_isolated(
          _h, sym, qty, entry, info[3].As<Napi::Number>().DoubleValue());
    }
    else
    {
      flox_account_open_position(_h, sym, qty, entry);
    }
  }
  void ClosePosition(const Napi::CallbackInfo& info)
  {
    flox_account_close_position(_h, info[0].As<Napi::Number>().Uint32Value());
  }
  Napi::Value PositionCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_account_position_count(_h));
  }
  void SetMark(const Napi::CallbackInfo& info)
  {
    const uint32_t sym = info[0].As<Napi::Number>().Uint32Value();
    const double price = info[1].As<Napi::Number>().DoubleValue();
    if (info.Length() >= 3 && info[2].IsNumber())
    {
      flox_account_set_mark_at(_h, sym, price,
                               info[2].As<Napi::Number>().Int64Value());
    }
    else
    {
      flox_account_set_mark(_h, sym, price);
    }
  }
  Napi::Value MarkTsFor(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(),
        static_cast<double>(flox_account_mark_ts(
            _h, info[0].As<Napi::Number>().Uint32Value())));
  }
  Napi::Value HasStaleMarks(const Napi::CallbackInfo& info)
  {
    return Napi::Boolean::New(
        info.Env(),
        flox_account_has_stale_marks(_h, info[0].As<Napi::Number>().Int64Value(),
                                     info[1].As<Napi::Number>().Int64Value()) != 0);
  }
  Napi::Value TotalNotional(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_account_total_notional(_h));
  }
  Napi::Value TotalUpnl(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), flox_account_total_unrealised_pnl(_h));
  }
  void RecordFill(const Napi::CallbackInfo& info)
  {
    const int64_t ts = info[0].As<Napi::Number>().Int64Value();
    const double notional = info[1].As<Napi::Number>().DoubleValue();
    if (info.Length() >= 3 && info[2].IsNumber())
    {
      flox_account_record_fill_ex(_h, ts, notional,
                                  info[2].As<Napi::Number>().Uint32Value());
    }
    else
    {
      flox_account_record_fill(_h, ts, notional);
    }
  }
  Napi::Value RollingNotionalBySymbol(const Napi::CallbackInfo& info)
  {
    const uint32_t n = flox_account_rolling_notional_by_symbol_size(_h);
    std::vector<uint32_t> syms(n);
    std::vector<double> totals(n);
    flox_account_rolling_notional_by_symbol_copy(_h, syms.data(),
                                                 totals.data(), n);
    Napi::Array arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; ++i)
    {
      Napi::Object pair = Napi::Object::New(info.Env());
      pair.Set("symbol", Napi::Number::New(info.Env(), syms[i]));
      pair.Set("notional", Napi::Number::New(info.Env(), totals[i]));
      arr.Set(i, pair);
    }
    return arr;
  }
  Napi::Value RollingNotional(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             flox_account_rolling_notional_30d(_h));
  }
  void ResetRolling(const Napi::CallbackInfo& info)
  {
    flox_account_reset_rolling(_h);
  }

  FloxAccountHandle _h{nullptr};
};

inline void registerAccount(Napi::Env env, Napi::Object exports)
{
  exports.Set("Account", AccountWrap::Init(env));
}

}  // namespace node_flox
