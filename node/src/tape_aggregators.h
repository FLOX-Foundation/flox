// node/src/tape_aggregators.h
//
// NAPI surface for the streaming tape aggregator framework
// (W14-T019). Five ObjectWrap classes thinly wrap the C ABI
// (`flox_*_aggregator_create` + `flox_*_read_result`); the matching
// `DataReader.run([...])` / `MergedTapeReader.run([...])` methods live
// on the reader wraps in data_ops.h and reach in via the public
// `collectAggregatorHandles` helper here.

#pragma once

#include <napi.h>

#include "flox/capi/flox_capi.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace node_flox
{

// ── Aggregator wrap helpers ─────────────────────────────────────────

namespace tape_agg_detail
{

inline FloxAggregatorEventFilter readEventFilter(const Napi::CallbackInfo& info, size_t idx,
                                                 FloxAggregatorEventFilter default_value)
{
  if (info.Length() <= idx || !info[idx].IsNumber())
  {
    return default_value;
  }
  int32_t raw = info[idx].As<Napi::Number>().Int32Value();
  switch (raw)
  {
    case FLOX_AGG_FILTER_TRADES:
    case FLOX_AGG_FILTER_BOOKS_ONLY:
    case FLOX_AGG_FILTER_BOTH:
      return static_cast<FloxAggregatorEventFilter>(raw);
    default:
      return default_value;
  }
}

inline std::vector<uint32_t> readSymbolFilter(const Napi::CallbackInfo& info, size_t idx)
{
  std::vector<uint32_t> out;
  if (info.Length() <= idx || !info[idx].IsArray())
  {
    return out;
  }
  auto arr = info[idx].As<Napi::Array>();
  out.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i)
  {
    Napi::Value v = arr[i];
    if (v.IsNumber())
    {
      out.push_back(v.As<Napi::Number>().Uint32Value());
    }
  }
  return out;
}

inline std::vector<int64_t> readInt64Array(Napi::Array arr)
{
  std::vector<int64_t> out;
  out.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i)
  {
    Napi::Value v = arr[i];
    if (v.IsBigInt())
    {
      bool lossless = false;
      out.push_back(v.As<Napi::BigInt>().Int64Value(&lossless));
    }
    else if (v.IsNumber())
    {
      out.push_back(v.As<Napi::Number>().Int64Value());
    }
  }
  return out;
}

inline std::vector<double> readDoubleArray(Napi::Array arr)
{
  std::vector<double> out;
  out.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i)
  {
    Napi::Value v = arr[i];
    if (v.IsNumber())
    {
      out.push_back(v.As<Napi::Number>().DoubleValue());
    }
  }
  return out;
}

}  // namespace tape_agg_detail

// ── EventTypeStatsAggregator ────────────────────────────────────────

class EventTypeStatsAggregatorWrap
    : public Napi::ObjectWrap<EventTypeStatsAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(
        env, "EventTypeStatsAggregator",
        {InstanceMethod("result", &EventTypeStatsAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  EventTypeStatsAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<EventTypeStatsAggregatorWrap>(info)
  {
    auto filter = tape_agg_detail::readEventFilter(info, 0, FLOX_AGG_FILTER_BOTH);
    auto sf = tape_agg_detail::readSymbolFilter(info, 1);
    _h = flox_event_type_stats_aggregator_create(filter, sf.empty() ? nullptr : sf.data(),
                                                 static_cast<uint32_t>(sf.size()));
  }
  ~EventTypeStatsAggregatorWrap()
  {
    if (_h)
    {
      flox_aggregator_destroy(_h);
    }
  }

  FloxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = flox_event_type_stats_read_result(_h, nullptr, 0);
    std::vector<FloxEventTypeStatsRow> rows(n);
    if (n > 0)
    {
      flox_event_type_stats_read_result(_h, rows.data(), n);
    }
    auto env = info.Env();
    auto arr = Napi::Array::New(env, n);
    for (uint32_t i = 0; i < n; ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("symbolId", Napi::Number::New(env, rows[i].symbol_id));
      o.Set("trades", Napi::BigInt::New(env, rows[i].trades));
      o.Set("bookSnapshots", Napi::BigInt::New(env, rows[i].book_snapshots));
      o.Set("bookDeltas", Napi::BigInt::New(env, rows[i].book_deltas));
      arr.Set(i, o);
    }
    return arr;
  }

  FloxAggregatorHandle _h = nullptr;
};

// ── BinCountAggregator ──────────────────────────────────────────────

class BinCountAggregatorWrap : public Napi::ObjectWrap<BinCountAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(env, "BinCountAggregator",
                                    {InstanceMethod("result", &BinCountAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  BinCountAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BinCountAggregatorWrap>(info)
  {
    int64_t bucket_ns = 0;
    if (info.Length() > 0)
    {
      if (info[0].IsBigInt())
      {
        bool lossless = false;
        bucket_ns = info[0].As<Napi::BigInt>().Int64Value(&lossless);
      }
      else if (info[0].IsNumber())
      {
        bucket_ns = info[0].As<Napi::Number>().Int64Value();
      }
    }
    uint8_t by_side = info.Length() > 1 && info[1].IsBoolean() && info[1].As<Napi::Boolean>().Value();
    uint8_t by_symbol = info.Length() > 2 && info[2].IsBoolean() && info[2].As<Napi::Boolean>().Value();
    auto filter = tape_agg_detail::readEventFilter(info, 3, FLOX_AGG_FILTER_BOTH);
    auto sf = tape_agg_detail::readSymbolFilter(info, 4);
    _h = flox_bin_count_aggregator_create(bucket_ns, by_side, by_symbol, filter,
                                          sf.empty() ? nullptr : sf.data(),
                                          static_cast<uint32_t>(sf.size()));
  }
  ~BinCountAggregatorWrap()
  {
    if (_h)
    {
      flox_aggregator_destroy(_h);
    }
  }

  FloxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = flox_bin_count_read_result(_h, nullptr, 0);
    std::vector<FloxBinCountRow> rows(n);
    if (n > 0)
    {
      flox_bin_count_read_result(_h, rows.data(), n);
    }
    auto env = info.Env();
    auto arr = Napi::Array::New(env, n);
    for (uint32_t i = 0; i < n; ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("bucketTsNs", Napi::BigInt::New(env, rows[i].bucket_ts_ns));
      o.Set("symbolId", Napi::Number::New(env, rows[i].symbol_id));
      o.Set("side", Napi::Number::New(env, rows[i].side));
      o.Set("count", Napi::BigInt::New(env, rows[i].count));
      arr.Set(i, o);
    }
    return arr;
  }

  FloxAggregatorHandle _h = nullptr;
};

// ── VolumeBinAggregator ─────────────────────────────────────────────

class VolumeBinAggregatorWrap : public Napi::ObjectWrap<VolumeBinAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(
        env, "VolumeBinAggregator",
        {InstanceMethod("result", &VolumeBinAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  VolumeBinAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<VolumeBinAggregatorWrap>(info)
  {
    int64_t bucket_ns = 0;
    if (info.Length() > 0)
    {
      if (info[0].IsBigInt())
      {
        bool lossless = false;
        bucket_ns = info[0].As<Napi::BigInt>().Int64Value(&lossless);
      }
      else if (info[0].IsNumber())
      {
        bucket_ns = info[0].As<Napi::Number>().Int64Value();
      }
    }
    uint8_t by_side = info.Length() > 1 && info[1].IsBoolean() && info[1].As<Napi::Boolean>().Value();
    uint8_t by_symbol = info.Length() > 2 && info[2].IsBoolean() && info[2].As<Napi::Boolean>().Value();
    auto filter = tape_agg_detail::readEventFilter(info, 3, FLOX_AGG_FILTER_TRADES);
    auto sf = tape_agg_detail::readSymbolFilter(info, 4);
    _h = flox_volume_bin_aggregator_create(bucket_ns, by_side, by_symbol, filter,
                                           sf.empty() ? nullptr : sf.data(),
                                           static_cast<uint32_t>(sf.size()));
  }
  ~VolumeBinAggregatorWrap()
  {
    if (_h)
    {
      flox_aggregator_destroy(_h);
    }
  }

  FloxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = flox_volume_bin_read_result(_h, nullptr, 0);
    std::vector<FloxVolumeBinRow> rows(n);
    if (n > 0)
    {
      flox_volume_bin_read_result(_h, rows.data(), n);
    }
    auto env = info.Env();
    auto arr = Napi::Array::New(env, n);
    for (uint32_t i = 0; i < n; ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("bucketTsNs", Napi::BigInt::New(env, rows[i].bucket_ts_ns));
      o.Set("symbolId", Napi::Number::New(env, rows[i].symbol_id));
      o.Set("side", Napi::Number::New(env, rows[i].side));
      o.Set("qtyRaw", Napi::BigInt::New(env, rows[i].qty_raw));
      arr.Set(i, o);
    }
    return arr;
  }

  FloxAggregatorHandle _h = nullptr;
};

// ── PeakAggregator ──────────────────────────────────────────────────

class PeakAggregatorWrap : public Napi::ObjectWrap<PeakAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(env, "PeakAggregator",
                                    {InstanceMethod("result", &PeakAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  PeakAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<PeakAggregatorWrap>(info)
  {
    std::vector<int64_t> windows;
    if (info.Length() > 0 && info[0].IsArray())
    {
      windows = tape_agg_detail::readInt64Array(info[0].As<Napi::Array>());
    }
    uint32_t top_n =
        info.Length() > 1 && info[1].IsNumber() ? info[1].As<Napi::Number>().Uint32Value() : 10;
    uint32_t oversample = info.Length() > 2 && info[2].IsNumber()
                              ? info[2].As<Napi::Number>().Uint32Value()
                              : 100;
    auto filter = tape_agg_detail::readEventFilter(info, 3, FLOX_AGG_FILTER_TRADES);
    auto sf = tape_agg_detail::readSymbolFilter(info, 4);
    _h = flox_peak_aggregator_create(windows.empty() ? nullptr : windows.data(),
                                     static_cast<uint32_t>(windows.size()), top_n,
                                     oversample, filter,
                                     sf.empty() ? nullptr : sf.data(),
                                     static_cast<uint32_t>(sf.size()));
  }
  ~PeakAggregatorWrap()
  {
    if (_h)
    {
      flox_aggregator_destroy(_h);
    }
  }

  FloxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = flox_peak_read_result(_h, nullptr, 0);
    std::vector<FloxPeakRow> rows(n);
    if (n > 0)
    {
      flox_peak_read_result(_h, rows.data(), n);
    }
    // Result shape: Array<{windowNs: bigint, peaks: Array<{count, startNs}>}>.
    // One outer entry per distinct window scale; peaks within each
    // entry are sorted by count descending (engine guarantees order).
    auto env = info.Env();
    std::vector<int64_t> seen_windows;
    std::vector<Napi::Array> per_window_arrays;
    for (uint32_t i = 0; i < n; ++i)
    {
      auto it = std::find(seen_windows.begin(), seen_windows.end(), rows[i].window_ns);
      Napi::Array arr;
      if (it == seen_windows.end())
      {
        arr = Napi::Array::New(env);
        seen_windows.push_back(rows[i].window_ns);
        per_window_arrays.push_back(arr);
      }
      else
      {
        arr = per_window_arrays[std::distance(seen_windows.begin(), it)];
      }
      auto entry = Napi::Object::New(env);
      entry.Set("count", Napi::BigInt::New(env, rows[i].count));
      entry.Set("startNs", Napi::BigInt::New(env, rows[i].start_ns));
      arr.Set(arr.Length(), entry);
    }
    auto outer = Napi::Array::New(env, seen_windows.size());
    for (uint32_t i = 0; i < seen_windows.size(); ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("windowNs", Napi::BigInt::New(env, seen_windows[i]));
      o.Set("peaks", per_window_arrays[i]);
      outer.Set(i, o);
    }
    return outer;
  }

  FloxAggregatorHandle _h = nullptr;
};

// ── QuantileAggregator ──────────────────────────────────────────────

class QuantileAggregatorWrap : public Napi::ObjectWrap<QuantileAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(
        env, "QuantileAggregator",
        {InstanceMethod("result", &QuantileAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  QuantileAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<QuantileAggregatorWrap>(info)
  {
    std::vector<int64_t> windows;
    if (info.Length() > 0 && info[0].IsArray())
    {
      windows = tape_agg_detail::readInt64Array(info[0].As<Napi::Array>());
    }
    std::vector<double> quantiles;
    if (info.Length() > 1 && info[1].IsArray())
    {
      quantiles = tape_agg_detail::readDoubleArray(info[1].As<Napi::Array>());
    }
    auto filter = tape_agg_detail::readEventFilter(info, 2, FLOX_AGG_FILTER_TRADES);
    auto sf = tape_agg_detail::readSymbolFilter(info, 3);
    _h = flox_quantile_aggregator_create(
        windows.empty() ? nullptr : windows.data(), static_cast<uint32_t>(windows.size()),
        quantiles.empty() ? nullptr : quantiles.data(),
        static_cast<uint32_t>(quantiles.size()), filter,
        sf.empty() ? nullptr : sf.data(), static_cast<uint32_t>(sf.size()));
  }
  ~QuantileAggregatorWrap()
  {
    if (_h)
    {
      flox_aggregator_destroy(_h);
    }
  }

  FloxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = flox_quantile_read_result(_h, nullptr, 0);
    std::vector<FloxQuantileRow> rows(n);
    if (n > 0)
    {
      flox_quantile_read_result(_h, rows.data(), n);
    }
    auto env = info.Env();
    // Result shape: Array<{windowNs: bigint, thresholds:
    // Array<{quantile: number, count: bigint}>}>. One outer entry per
    // distinct window; thresholds within each entry are ordered as
    // the engine returned them (ascending by quantile).
    std::vector<int64_t> seen_windows;
    std::vector<Napi::Array> per_window_arrays;
    for (uint32_t i = 0; i < n; ++i)
    {
      auto it = std::find(seen_windows.begin(), seen_windows.end(), rows[i].window_ns);
      Napi::Array arr;
      if (it == seen_windows.end())
      {
        arr = Napi::Array::New(env);
        seen_windows.push_back(rows[i].window_ns);
        per_window_arrays.push_back(arr);
      }
      else
      {
        arr = per_window_arrays[std::distance(seen_windows.begin(), it)];
      }
      auto entry = Napi::Object::New(env);
      entry.Set("quantile", Napi::Number::New(env, rows[i].quantile));
      entry.Set("count", Napi::BigInt::New(env, rows[i].count));
      arr.Set(arr.Length(), entry);
    }
    auto outer = Napi::Array::New(env, seen_windows.size());
    for (uint32_t i = 0; i < seen_windows.size(); ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("windowNs", Napi::BigInt::New(env, seen_windows[i]));
      o.Set("thresholds", per_window_arrays[i]);
      outer.Set(i, o);
    }
    return outer;
  }

  FloxAggregatorHandle _h = nullptr;
};

// ── Public helpers + registration ──────────────────────────────────

// Walk a JS array of aggregator wraps and produce a flat list of
// FloxAggregatorHandle pointers. Returns true on success; sets a JS
// TypeError and returns false if any element isn't one of the five
// recognised wrap classes.
inline bool collectAggregatorHandles(Napi::Env env, Napi::Array js_aggregators,
                                     std::vector<FloxAggregatorHandle>& out)
{
  out.clear();
  out.reserve(js_aggregators.Length());
  for (uint32_t i = 0; i < js_aggregators.Length(); ++i)
  {
    Napi::Value v = js_aggregators[i];
    if (!v.IsObject())
    {
      Napi::TypeError::New(env, "Aggregator array element is not an object")
          .ThrowAsJavaScriptException();
      return false;
    }
    auto obj = v.As<Napi::Object>();
    FloxAggregatorHandle h = nullptr;
    if (obj.InstanceOf(EventTypeStatsAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<EventTypeStatsAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else if (obj.InstanceOf(BinCountAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<BinCountAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else if (obj.InstanceOf(VolumeBinAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<VolumeBinAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else if (obj.InstanceOf(PeakAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<PeakAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else if (obj.InstanceOf(QuantileAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<QuantileAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else
    {
      Napi::TypeError::New(
          env,
          "Element is not a recognised tape aggregator wrap "
          "(EventTypeStats/BinCount/VolumeBin/Peak/Quantile)")
          .ThrowAsJavaScriptException();
      return false;
    }
    out.push_back(h);
  }
  return true;
}

inline void registerTapeAggregators(Napi::Env env, Napi::Object exports)
{
  exports.Set("EventTypeStatsAggregator", EventTypeStatsAggregatorWrap::Init(env));
  exports.Set("BinCountAggregator", BinCountAggregatorWrap::Init(env));
  exports.Set("VolumeBinAggregator", VolumeBinAggregatorWrap::Init(env));
  exports.Set("PeakAggregator", PeakAggregatorWrap::Init(env));
  exports.Set("QuantileAggregator", QuantileAggregatorWrap::Init(env));

  // Mirror the C ABI enum values so JS callers can pass them positionally.
  auto filterEnum = Napi::Object::New(env);
  filterEnum.Set("Trades", Napi::Number::New(env, FLOX_AGG_FILTER_TRADES));
  filterEnum.Set("BooksOnly", Napi::Number::New(env, FLOX_AGG_FILTER_BOOKS_ONLY));
  filterEnum.Set("Both", Napi::Number::New(env, FLOX_AGG_FILTER_BOTH));
  exports.Set("AggregatorEventFilter", filterEnum);
}

}  // namespace node_flox
