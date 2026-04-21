// node/src/aggregators.h -- bar aggregation functions

#pragma once
#include <napi.h>
#include "flox/capi/flox_capi.h"
#include <cstring>
#include <vector>

namespace node_flox
{

// Convert FloxBar array to JS array of objects
inline Napi::Value barsToJs(Napi::Env env, const FloxBar* bars, uint32_t n)
{
  auto arr = Napi::Array::New(env, n);
  for (uint32_t i = 0; i < n; i++)
  {
    auto o = Napi::Object::New(env);
    o.Set("startTimeNs", (double)bars[i].start_time_ns);
    o.Set("endTimeNs", (double)bars[i].end_time_ns);
    o.Set("open", (double)bars[i].open_raw / 1e8);
    o.Set("high", (double)bars[i].high_raw / 1e8);
    o.Set("low", (double)bars[i].low_raw / 1e8);
    o.Set("close", (double)bars[i].close_raw / 1e8);
    o.Set("volume", (double)bars[i].volume_raw / 1e8);
    o.Set("buyVolume", (double)bars[i].buy_volume_raw / 1e8);
    o.Set("tradeCount", bars[i].trade_count);
    arr.Set(i, o);
  }
  return arr;
}

// Common extraction of trade arrays
struct TradeArrays
{
  std::vector<int64_t> ts;
  const double *px, *qty;
  const uint8_t* ib;
  size_t n;
};

inline TradeArrays extractTrades(const Napi::CallbackInfo& info)
{
  auto ts = info[0].As<Napi::Float64Array>();
  auto px = info[1].As<Napi::Float64Array>();
  auto qty = info[2].As<Napi::Float64Array>();
  auto ib = info[3].As<Napi::Uint8Array>();
  size_t n = ts.ElementLength();

  // Convert float64 timestamps to int64
  std::vector<int64_t> tsVec(n);
  for (size_t i = 0; i < n; i++)
    tsVec[i] = static_cast<int64_t>(ts[i]);

  return {std::move(tsVec), px.Data(), qty.Data(), ib.Data(), n};
}

inline Napi::Value agg_time_bars(const Napi::CallbackInfo& info)
{
  auto t = extractTrades(info);
  double interval = info[4].As<Napi::Number>().DoubleValue();
  uint32_t maxBars = t.n;
  std::vector<FloxBar> bars(maxBars);
  uint32_t count = flox_aggregate_time_bars(t.ts.data(), t.px, t.qty, t.ib, t.n, interval, bars.data(), maxBars);
  return barsToJs(info.Env(), bars.data(), count);
}

inline Napi::Value agg_tick_bars(const Napi::CallbackInfo& info)
{
  auto t = extractTrades(info);
  uint32_t tickCount = info[4].As<Napi::Number>().Uint32Value();
  uint32_t maxBars = t.n / tickCount + 1;
  std::vector<FloxBar> bars(maxBars);
  uint32_t count = flox_aggregate_tick_bars(t.ts.data(), t.px, t.qty, t.ib, t.n, tickCount, bars.data(), maxBars);
  return barsToJs(info.Env(), bars.data(), count);
}

inline Napi::Value agg_volume_bars(const Napi::CallbackInfo& info)
{
  auto t = extractTrades(info);
  double threshold = info[4].As<Napi::Number>().DoubleValue();
  uint32_t maxBars = t.n;
  std::vector<FloxBar> bars(maxBars);
  uint32_t count = flox_aggregate_volume_bars(t.ts.data(), t.px, t.qty, t.ib, t.n, threshold, bars.data(), maxBars);
  return barsToJs(info.Env(), bars.data(), count);
}

inline Napi::Value agg_range_bars(const Napi::CallbackInfo& info)
{
  auto t = extractTrades(info);
  double range = info[4].As<Napi::Number>().DoubleValue();
  uint32_t maxBars = t.n;
  std::vector<FloxBar> bars(maxBars);
  uint32_t count = flox_aggregate_range_bars(t.ts.data(), t.px, t.qty, t.ib, t.n, range, bars.data(), maxBars);
  return barsToJs(info.Env(), bars.data(), count);
}

inline Napi::Value agg_renko_bars(const Napi::CallbackInfo& info)
{
  auto t = extractTrades(info);
  double brick = info[4].As<Napi::Number>().DoubleValue();
  uint32_t maxBars = t.n;
  std::vector<FloxBar> bars(maxBars);
  uint32_t count = flox_aggregate_renko_bars(t.ts.data(), t.px, t.qty, t.ib, t.n, brick, bars.data(), maxBars);
  return barsToJs(info.Env(), bars.data(), count);
}

inline Napi::Value agg_heikin_ashi_bars(const Napi::CallbackInfo& info)
{
  auto t = extractTrades(info);
  double interval = info[4].As<Napi::Number>().DoubleValue();
  uint32_t maxBars = t.n;
  std::vector<FloxBar> bars(maxBars);
  uint32_t count = flox_aggregate_heikin_ashi_bars(t.ts.data(), t.px, t.qty, t.ib, t.n, interval, bars.data(), maxBars);
  return barsToJs(info.Env(), bars.data(), count);
}

inline void registerAggregators(Napi::Env env, Napi::Object exports)
{
  exports.Set("aggregateTimeBars", Napi::Function::New(env, agg_time_bars));
  exports.Set("aggregateTickBars", Napi::Function::New(env, agg_tick_bars));
  exports.Set("aggregateVolumeBars", Napi::Function::New(env, agg_volume_bars));
  exports.Set("aggregateRangeBars", Napi::Function::New(env, agg_range_bars));
  exports.Set("aggregateRenkoBars", Napi::Function::New(env, agg_renko_bars));
  exports.Set("aggregateHeikinAshiBars", Napi::Function::New(env, agg_heikin_ashi_bars));
}

}  // namespace node_flox
