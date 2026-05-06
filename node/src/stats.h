// node/src/stats.h -- statistical functions

#pragma once
#include <napi.h>
#include <cstring>
#include <vector>
#include "flox/capi/flox_capi.h"

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
  o.Set("lower", lo);
  o.Set("median", med);
  o.Set("upper", hi);
  return o;
}

inline Napi::Value stat_permutation_test(const Napi::CallbackInfo& info)
{
  auto g1 = info[0].As<Napi::Float64Array>();
  auto g2 = info[1].As<Napi::Float64Array>();
  uint32_t n = info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 10000;
  return Napi::Number::New(info.Env(), flox_stat_permutation_test(g1.Data(), g1.ElementLength(), g2.Data(), g2.ElementLength(), n));
}

// White's reality check.
//
// Args (positional):
//   0: returns Float64Array — flat row-major K*T matrix of EXCESS returns
//      (each strategy's series concatenated; caller is responsible for
//      benchmark adjustment).
//   1: numStrategies (number)
//   2: numPeriods (number)
//   3: numBootstrap (number, optional, default 10000)
//   4: avgBlockSize (number, optional, default 0 → auto sqrt(T))
//
// Returns: { p_value: number, best_stat: number, best_index: number }.
inline Napi::Value stat_whites_reality_check(const Napi::CallbackInfo& info)
{
  auto returns = info[0].As<Napi::Float64Array>();
  size_t K = info[1].As<Napi::Number>().Uint32Value();
  size_t T = info[2].As<Napi::Number>().Uint32Value();
  uint32_t numBootstrap = info.Length() > 3 ? info[3].As<Napi::Number>().Uint32Value() : 10000u;
  double avgBlock = info.Length() > 4 ? info[4].As<Napi::Number>().DoubleValue() : 0.0;
  if (returns.ElementLength() < K * T)
  {
    Napi::TypeError::New(info.Env(),
                         "returns array shorter than K*T")
        .ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }
  double pValue = 1.0;
  double bestStat = 0.0;
  int32_t bestIndex = -1;
  flox_stat_whites_reality_check(returns.Data(), K, T, numBootstrap, avgBlock,
                                 &pValue, &bestStat, &bestIndex);
  auto o = Napi::Object::New(info.Env());
  o.Set("p_value", pValue);
  o.Set("best_stat", bestStat);
  o.Set("best_index", bestIndex);
  return o;
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
  {
    out[i] = static_cast<double>(sl[i - 1] + ss[i - 1]) * lr[i];
  }
  return out;
}

inline Napi::Value stat_trade_pnl(const Napi::CallbackInfo& info)
{
  auto sl = info[0].As<Napi::Int8Array>();
  auto ss = info[1].As<Napi::Int8Array>();
  auto lr = info[2].As<Napi::Float64Array>();
  size_t n = sl.ElementLength();
  std::vector<double> trades;
  double pnl = 0;
  int8_t prev = 0;
  bool inTrade = false;
  for (size_t i = 1; i < n; ++i)
  {
    int8_t pos = sl[i - 1] + ss[i - 1];
    double ret = static_cast<double>(pos) * lr[i];
    if (pos != prev)
    {
      if (inTrade)
      {
        trades.push_back(pnl);
        pnl = 0;
      }
      inTrade = pos != 0;
    }
    if (pos != 0)
    {
      pnl += ret;
    }
    prev = pos;
  }
  if (inTrade)
  {
    trades.push_back(pnl);
  }
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
  exports.Set("whitesRealityCheck", Napi::Function::New(env, stat_whites_reality_check));
  exports.Set("barReturns", Napi::Function::New(env, stat_bar_returns));
  exports.Set("tradePnl", Napi::Function::New(env, stat_trade_pnl));
}

}  // namespace node_flox
