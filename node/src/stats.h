// node/src/stats.h -- statistical functions

#pragma once
#include <napi.h>
#include "flox/capi/flox_capi.h"
#include <cstring>
#include <vector>

namespace node_flox
{

inline std::vector<double> toVec(Napi::Float64Array& a) { return {a.Data(), a.Data() + a.ElementLength()}; }

inline Napi::Value stat_correlation(const Napi::CallbackInfo& info)
{
  auto x = info[0].As<Napi::Float64Array>();
  auto y = info[1].As<Napi::Float64Array>();
  size_t n = std::min(x.ElementLength(), y.ElementLength());
  return Napi::Number::New(info.Env(), flox_stat_correlation(x.Data(), y.Data(), n));
}

inline Napi::Value stat_profit_factor(const Napi::CallbackInfo& info)
{
  auto a = info[0].As<Napi::Float64Array>();
  return Napi::Number::New(info.Env(), flox_stat_profit_factor(a.Data(), a.ElementLength()));
}

inline Napi::Value stat_win_rate(const Napi::CallbackInfo& info)
{
  auto a = info[0].As<Napi::Float64Array>();
  return Napi::Number::New(info.Env(), flox_stat_win_rate(a.Data(), a.ElementLength()));
}

inline Napi::Value stat_bootstrap_ci(const Napi::CallbackInfo& info)
{
  auto a = info[0].As<Napi::Float64Array>();
  double conf = info.Length() > 1 ? info[1].As<Napi::Number>().DoubleValue() : 0.95;
  uint32_t samples = info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 10000;
  double lo, med, hi;
  flox_stat_bootstrap_ci(a.Data(), a.ElementLength(), conf, samples, &lo, &med, &hi);
  auto o = Napi::Object::New(info.Env());
  o.Set("lower", lo); o.Set("median", med); o.Set("upper", hi);
  return o;
}

inline Napi::Value stat_permutation_test(const Napi::CallbackInfo& info)
{
  auto g1 = info[0].As<Napi::Float64Array>();
  auto g2 = info[1].As<Napi::Float64Array>();
  uint32_t n = info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 10000;
  return Napi::Number::New(info.Env(), flox_stat_permutation_test(g1.Data(), g1.ElementLength(), g2.Data(), g2.ElementLength(), n));
}

inline Napi::Value stat_bar_returns(const Napi::CallbackInfo& info)
{
  auto sl = info[0].As<Napi::Int8Array>();
  auto ss = info[1].As<Napi::Int8Array>();
  auto lr = info[2].As<Napi::Float64Array>();
  size_t n = sl.ElementLength();
  auto out = Napi::Float64Array::New(info.Env(), n);
  out[0] = 0.0;
  for (size_t i = 1; i < n; ++i)
    out[i] = static_cast<double>(sl[i - 1] + ss[i - 1]) * lr[i];
  return out;
}

inline Napi::Value stat_trade_pnl(const Napi::CallbackInfo& info)
{
  auto sl = info[0].As<Napi::Int8Array>();
  auto ss = info[1].As<Napi::Int8Array>();
  auto lr = info[2].As<Napi::Float64Array>();
  size_t n = sl.ElementLength();
  std::vector<double> trades;
  double pnl = 0; int8_t prev = 0; bool inTrade = false;
  for (size_t i = 1; i < n; ++i) {
    int8_t pos = sl[i-1] + ss[i-1];
    double ret = static_cast<double>(pos) * lr[i];
    if (pos != prev) { if (inTrade) { trades.push_back(pnl); pnl = 0; } inTrade = pos != 0; }
    if (pos != 0) pnl += ret;
    prev = pos;
  }
  if (inTrade) trades.push_back(pnl);
  auto out = Napi::Float64Array::New(info.Env(), trades.size());
  std::memcpy(out.Data(), trades.data(), trades.size() * sizeof(double));
  return out;
}

inline void registerStats(Napi::Env env, Napi::Object exports)
{
  exports.Set("correlation", Napi::Function::New(env, stat_correlation));
  exports.Set("profitFactor", Napi::Function::New(env, stat_profit_factor));
  exports.Set("winRate", Napi::Function::New(env, stat_win_rate));
  exports.Set("bootstrapCI", Napi::Function::New(env, stat_bootstrap_ci));
  exports.Set("permutationTest", Napi::Function::New(env, stat_permutation_test));
  exports.Set("barReturns", Napi::Function::New(env, stat_bar_returns));
  exports.Set("tradePnl", Napi::Function::New(env, stat_trade_pnl));
}

}  // namespace node_flox
