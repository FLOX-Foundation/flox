// node/src/tape_diff.h -- tape diff wrapper for Node.js.
//
// Exposes a single function `flox.tapeDiff(leftPath, rightPath, opts?)`
// that returns a JS object matching the shape of the Python
// TapeDiff dataclass. The walk runs entirely in the C++ engine
// through the C ABI; this is purely marshalling.

#pragma once

#include <napi.h>

#include "error_translator.h"
#include "flox/capi/flox_capi.h"

#include <cstdint>
#include <string>

namespace node_flox
{

namespace detail_diff
{

inline Napi::Object tradeToJs(Napi::Env env, const FloxTapeDiffTrade& t)
{
  Napi::Object o = Napi::Object::New(env);
  o.Set("exchangeTsNs", Napi::Number::New(env, static_cast<double>(t.exchange_ts_ns)));
  o.Set("symbolId", Napi::Number::New(env, t.symbol_id));
  o.Set("priceRaw", Napi::Number::New(env, static_cast<double>(t.price_raw)));
  o.Set("qtyRaw", Napi::Number::New(env, static_cast<double>(t.qty_raw)));
  o.Set("side", Napi::Number::New(env, t.side));
  return o;
}

}  // namespace detail_diff

inline Napi::Value tapeDiff(const Napi::CallbackInfo& info)
{
  return tryFlox(info.Env(), [&]() -> Napi::Value
                 {
    auto env = info.Env();
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString())
    {
      throw flox::FloxError("E_VAL_002",
          "tapeDiff: first two arguments must be tape directory paths.");
    }
    std::string left = info[0].As<Napi::String>().Utf8Value();
    std::string right = info[1].As<Napi::String>().Utf8Value();
    uint32_t max_mismatches = 16;
    int64_t tolerance_ns = 0;
    if (info.Length() >= 3 && info[2].IsObject())
    {
      auto opts = info[2].As<Napi::Object>();
      if (opts.Has("maxMismatches"))
      {
        max_mismatches = opts.Get("maxMismatches").As<Napi::Number>().Uint32Value();
      }
      if (opts.Has("fieldToleranceNs"))
      {
        tolerance_ns = opts.Get("fieldToleranceNs").As<Napi::Number>().Int64Value();
      }
    }

    FloxTapeDiffHandle h = flox_tape_diff_create(left.c_str(), right.c_str(),
                                                  max_mismatches, tolerance_ns);
    if (!h)
    {
      throw flox::FloxError("E_IO_001",
          "tapeDiff: failed to read tape directory(ies).");
    }

    Napi::Object out = Napi::Object::New(env);
    out.Set("leftPath", Napi::String::New(env, left));
    out.Set("rightPath", Napi::String::New(env, right));
    out.Set("leftCount", Napi::Number::New(env,
        static_cast<double>(flox_tape_diff_left_count(h))));
    out.Set("rightCount", Napi::Number::New(env,
        static_cast<double>(flox_tape_diff_right_count(h))));

    uint64_t div_idx = 0;
    if (flox_tape_diff_first_divergence(h, &div_idx))
    {
      out.Set("firstDivergenceIndex", Napi::Number::New(env, static_cast<double>(div_idx)));
    }
    else
    {
      out.Set("firstDivergenceIndex", env.Null());
    }
    out.Set("equal", Napi::Boolean::New(env, flox_tape_diff_equal(h) != 0));

    const uint64_t mcount = flox_tape_diff_mismatch_count(h);
    Napi::Array mlist = Napi::Array::New(env, static_cast<size_t>(mcount));
    if (mcount > 0)
    {
      std::vector<FloxTapeDiffMismatch> buf(mcount);
      flox_tape_diff_copy_mismatches(h, buf.data(), mcount);
      for (uint64_t i = 0; i < mcount; ++i)
      {
        Napi::Object entry = Napi::Object::New(env);
        entry.Set("index", Napi::Number::New(env, static_cast<double>(buf[i].index)));
        entry.Set("left", detail_diff::tradeToJs(env, buf[i].left));
        entry.Set("right", detail_diff::tradeToJs(env, buf[i].right));
        mlist.Set(static_cast<uint32_t>(i), entry);
      }
    }
    out.Set("mismatches", mlist);

    flox_tape_diff_destroy(h);
    return out; });
}

inline void registerTapeDiff(Napi::Env env, Napi::Object exports)
{
  exports.Set("tapeDiff",
              Napi::Function::New(env, &tapeDiff, "tapeDiff"));
}

}  // namespace node_flox
