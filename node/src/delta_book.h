// node/src/delta_book.h -- DeltaBookEncoder / DeltaBookReplayer wrappers.
//
// Encode L2 snapshots as anchor snapshots plus deltas; replay the
// stream back into reconstructed snapshots. Both classes wrap the
// FloxDeltaBookEncoderHandle / FloxDeltaBookReplayerHandle through
// the C ABI and translate level arrays to / from JS objects.

#pragma once

#include <napi.h>

#include "error_translator.h"
#include "flox/capi/flox_capi.h"

#include <cstdint>
#include <vector>

namespace node_flox
{

namespace detail_db
{

inline std::vector<FloxBookLevel> readLevels(const Napi::Array& arr)
{
  std::vector<FloxBookLevel> out;
  out.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i)
  {
    auto entry = arr.Get(i).As<Napi::Object>();
    FloxBookLevel l{};
    l.price_raw = entry.Get("priceRaw").As<Napi::Number>().Int64Value();
    l.quantity_raw = entry.Get("qtyRaw").As<Napi::Number>().Int64Value();
    out.push_back(l);
  }
  return out;
}

inline Napi::Array levelsToJs(Napi::Env env, const std::vector<FloxBookLevel>& levels)
{
  Napi::Array out = Napi::Array::New(env, levels.size());
  for (size_t i = 0; i < levels.size(); ++i)
  {
    Napi::Object o = Napi::Object::New(env);
    o.Set("priceRaw", Napi::Number::New(env, static_cast<double>(levels[i].price_raw)));
    o.Set("qtyRaw", Napi::Number::New(env, static_cast<double>(levels[i].quantity_raw)));
    out.Set(static_cast<uint32_t>(i), o);
  }
  return out;
}

}  // namespace detail_db

class DeltaBookEncoderWrap : public Napi::ObjectWrap<DeltaBookEncoderWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "DeltaBookEncoder",
                       {InstanceMethod("encode", &DeltaBookEncoderWrap::Encode),
                        InstanceMethod("reset", &DeltaBookEncoderWrap::Reset),
                        InstanceMethod("resetAll", &DeltaBookEncoderWrap::ResetAll)});
  }

  DeltaBookEncoderWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<DeltaBookEncoderWrap>(info)
  {
    uint32_t anchor_every = 100;
    if (info.Length() > 0 && info[0].IsObject())
    {
      auto opts = info[0].As<Napi::Object>();
      if (opts.Has("anchorEvery"))
      {
        anchor_every = opts.Get("anchorEvery").As<Napi::Number>().Uint32Value();
      }
    }
    _h = flox_delta_book_encoder_create(anchor_every);
    if (!_h)
    {
      auto err = Napi::Error::New(info.Env(), "DeltaBookEncoder: construction failed");
      err.Value().Set("code", Napi::String::New(info.Env(), "E_VAL_002"));
      err.Value().Set("name", Napi::String::New(info.Env(), "FloxError"));
      throw err;
    }
  }
  ~DeltaBookEncoderWrap()
  {
    if (_h)
    {
      flox_delta_book_encoder_destroy(_h);
    }
  }

 private:
  Napi::Value Encode(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsArray() || !info[2].IsArray())
    {
      Napi::TypeError::New(env, "encode(symbolId, bids, asks)").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    uint32_t sym = info[0].As<Napi::Number>().Uint32Value();
    auto bids = detail_db::readLevels(info[1].As<Napi::Array>());
    auto asks = detail_db::readLevels(info[2].As<Napi::Array>());
    uint8_t is_delta = 0;
    uint64_t bcnt = 0, acnt = 0;
    flox_delta_book_encoder_encode(_h, sym,
                                   bids.empty() ? nullptr : bids.data(), bids.size(),
                                   asks.empty() ? nullptr : asks.data(), asks.size(),
                                   &is_delta, &bcnt, &acnt);
    std::vector<FloxBookLevel> out_bids(bcnt);
    std::vector<FloxBookLevel> out_asks(acnt);
    if (bcnt > 0)
    {
      flox_delta_book_encoder_copy_bids(_h, out_bids.data(), bcnt);
    }
    if (acnt > 0)
    {
      flox_delta_book_encoder_copy_asks(_h, out_asks.data(), acnt);
    }
    Napi::Object result = Napi::Object::New(env);
    result.Set("isDelta", Napi::Boolean::New(env, is_delta != 0));
    result.Set("bids", detail_db::levelsToJs(env, out_bids));
    result.Set("asks", detail_db::levelsToJs(env, out_asks));
    return result;
  }

  void Reset(const Napi::CallbackInfo& info)
  {
    flox_delta_book_encoder_reset(_h, info[0].As<Napi::Number>().Uint32Value());
  }

  void ResetAll(const Napi::CallbackInfo&)
  {
    flox_delta_book_encoder_reset_all(_h);
  }

  FloxDeltaBookEncoderHandle _h{nullptr};
};

class DeltaBookReplayerWrap : public Napi::ObjectWrap<DeltaBookReplayerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "DeltaBookReplayer",
                       {InstanceMethod("apply", &DeltaBookReplayerWrap::Apply),
                        InstanceMethod("reset", &DeltaBookReplayerWrap::Reset),
                        InstanceMethod("resetAll", &DeltaBookReplayerWrap::ResetAll)});
  }

  DeltaBookReplayerWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<DeltaBookReplayerWrap>(info)
  {
    _h = flox_delta_book_replayer_create();
  }
  ~DeltaBookReplayerWrap()
  {
    if (_h)
    {
      flox_delta_book_replayer_destroy(_h);
    }
  }

 private:
  Napi::Value Apply(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() < 4)
    {
      Napi::TypeError::New(env, "apply(type, symbolId, bids, asks)").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    uint8_t type = static_cast<uint8_t>(info[0].As<Napi::Number>().Uint32Value());
    uint32_t sym = info[1].As<Napi::Number>().Uint32Value();
    auto bids = detail_db::readLevels(info[2].As<Napi::Array>());
    auto asks = detail_db::readLevels(info[3].As<Napi::Array>());
    uint64_t bcnt = 0, acnt = 0;
    flox_delta_book_replayer_apply(_h, type, sym,
                                   bids.empty() ? nullptr : bids.data(), bids.size(),
                                   asks.empty() ? nullptr : asks.data(), asks.size(),
                                   &bcnt, &acnt);
    std::vector<FloxBookLevel> out_bids(bcnt);
    std::vector<FloxBookLevel> out_asks(acnt);
    if (bcnt > 0)
    {
      flox_delta_book_replayer_copy_bids(_h, out_bids.data(), bcnt);
    }
    if (acnt > 0)
    {
      flox_delta_book_replayer_copy_asks(_h, out_asks.data(), acnt);
    }
    Napi::Object result = Napi::Object::New(env);
    result.Set("bids", detail_db::levelsToJs(env, out_bids));
    result.Set("asks", detail_db::levelsToJs(env, out_asks));
    return result;
  }

  void Reset(const Napi::CallbackInfo& info)
  {
    flox_delta_book_replayer_reset(_h, info[0].As<Napi::Number>().Uint32Value());
  }

  void ResetAll(const Napi::CallbackInfo&)
  {
    if (_h)
    {
      flox_delta_book_replayer_destroy(_h);
      _h = flox_delta_book_replayer_create();
    }
  }

  FloxDeltaBookReplayerHandle _h{nullptr};
};

inline void registerDeltaBook(Napi::Env env, Napi::Object exports)
{
  exports.Set("DeltaBookEncoder", DeltaBookEncoderWrap::Init(env));
  exports.Set("DeltaBookReplayer", DeltaBookReplayerWrap::Init(env));
}

}  // namespace node_flox
