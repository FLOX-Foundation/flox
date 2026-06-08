// node/src/amm_curve.h -- exact AMM pool curves (price a DEX swap) for Node.js.
//
// A curve is an opaque handle (a Napi::External over the C ABI's FloxCurveHandle,
// with a finalizer that frees it). Amounts are native BigInt -- lossless 256-bit --
// crossing through the dex_amount limb helpers. Construct per venue with the pool's
// parameters, then price a swap exactly:
//
//   const pool = flox.ammCurveConstantProduct(r0, r1, 997, 1000);
//   const out  = flox.ammCurveAmountOut(pool, 0, 1, 10n ** 18n);  // to the wei
//
// ammCurveAmountOut does not move the pool; ammCurveApplySwap moves it.

#pragma once

#include <napi.h>

#include "dex_amount.h"
#include "flox/capi/flox_capi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace node_flox
{

namespace detail_curve
{

inline std::string bigintToDecimal(Napi::Env env, const Napi::BigInt& b)
{
  uint64_t words[4];
  int sign = 0;
  if (!detail_dex::bigintToWords(b, words, &sign) || sign != 0)
  {
    throw Napi::RangeError::New(env, "amm curve: amount must be in [0, 2^256)");
  }
  char dec[96] = {0};
  if (!flox_u256_from_words(words, dec, sizeof(dec)))
  {
    throw Napi::Error::New(env, "amm curve: amount conversion failed");
  }
  return dec;
}

inline Napi::BigInt decimalToBigint(Napi::Env env, const char* dec)
{
  uint64_t words[4];
  if (!flox_u256_to_words(dec, words))
  {
    throw Napi::Error::New(env, "amm curve: result conversion failed");
  }
  return detail_dex::wordsToBigInt(env, 0, words);
}

inline Napi::Value wrapHandle(Napi::Env env, FloxCurveHandle h)
{
  if (h == nullptr)
  {
    throw Napi::Error::New(env, "amm curve: construction failed (bad parameter)");
  }
  return Napi::External<void>::New(env, h, [](Napi::Env, void* data)
                                   { flox_curve_destroy(data); });
}

inline FloxCurveHandle unwrapHandle(const Napi::Value& v)
{
  return v.As<Napi::External<void>>().Data();
}

inline std::string opOut(Napi::Env env,
                         uint8_t (*op)(FloxCurveHandle, size_t, size_t, const char*, char*, size_t),
                         FloxCurveHandle c, size_t i, size_t j, const std::string& amountIn)
{
  char out[96] = {0};
  if (!op(c, i, j, amountIn.c_str(), out, sizeof(out)))
  {
    throw Napi::Error::New(env, "amm curve: swap failed (bad index or amount)");
  }
  return out;
}

}  // namespace detail_curve

inline Napi::Value ammCurveConstantProduct(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  const std::string r0 = detail_curve::bigintToDecimal(env, info[0].As<Napi::BigInt>());
  const std::string r1 = detail_curve::bigintToDecimal(env, info[1].As<Napi::BigInt>());
  const auto feeNum = info[2].As<Napi::Number>().Int64Value();
  const auto feeDen = info[3].As<Napi::Number>().Int64Value();
  return detail_curve::wrapHandle(
      env, flox_curve_constant_product(r0.c_str(), r1.c_str(), static_cast<uint64_t>(feeNum),
                                       static_cast<uint64_t>(feeDen)));
}

inline Napi::Value ammCurveRaydiumCp(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  const std::string r0 = detail_curve::bigintToDecimal(env, info[0].As<Napi::BigInt>());
  const std::string r1 = detail_curve::bigintToDecimal(env, info[1].As<Napi::BigInt>());
  const auto tradeFee = info[2].As<Napi::Number>().Int64Value();
  const auto creatorFee = info.Length() > 3 ? info[3].As<Napi::Number>().Int64Value() : 0;
  const bool onInput = info.Length() > 4 ? info[4].As<Napi::Boolean>().Value() : true;
  return detail_curve::wrapHandle(
      env, flox_curve_raydium_cp(r0.c_str(), r1.c_str(), static_cast<uint64_t>(tradeFee),
                                 static_cast<uint64_t>(creatorFee), onInput ? 1 : 0));
}

inline Napi::Value ammCurveUniswapV3(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  const std::string sqrtP = detail_curve::bigintToDecimal(env, info[0].As<Napi::BigInt>());
  const std::string liq = detail_curve::bigintToDecimal(env, info[1].As<Napi::BigInt>());
  const auto feePips = info[2].As<Napi::Number>().Uint32Value();

  // ticks: an array of [sqrtRatio: bigint, liquidityNet: bigint]; signed net rides in
  // i256, so its sign is the BigInt's sign.
  std::vector<std::string> sqrtStore;
  std::vector<std::string> netStore;
  if (info.Length() > 3 && info[3].IsArray())
  {
    Napi::Array arr = info[3].As<Napi::Array>();
    for (uint32_t k = 0; k < arr.Length(); ++k)
    {
      Napi::Array pair = arr.Get(k).As<Napi::Array>();
      sqrtStore.push_back(detail_curve::bigintToDecimal(env, pair.Get(0u).As<Napi::BigInt>()));
      // liquidity net can be negative: format with sign from the limb words.
      uint64_t w[4];
      int sign = 0;
      detail_dex::bigintToWords(pair.Get(1u).As<Napi::BigInt>(), w, &sign);
      char mag[96] = {0};
      flox_u256_from_words(w, mag, sizeof(mag));
      netStore.push_back((sign != 0 && std::string(mag) != "0") ? std::string("-") + mag : mag);
    }
  }
  std::vector<const char*> sqrtPtrs;
  std::vector<const char*> netPtrs;
  for (const std::string& s : sqrtStore)
  {
    sqrtPtrs.push_back(s.c_str());
  }
  for (const std::string& s : netStore)
  {
    netPtrs.push_back(s.c_str());
  }
  return detail_curve::wrapHandle(
      env, flox_curve_uniswap_v3(sqrtP.c_str(), liq.c_str(), feePips, sqrtPtrs.data(),
                                 netPtrs.data(), sqrtPtrs.size()));
}

inline Napi::Value ammCurveTokenCount(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(info.Env(),
                           static_cast<double>(flox_curve_token_count(
                               detail_curve::unwrapHandle(info[0]))));
}

inline Napi::Value ammCurveAmountOut(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  FloxCurveHandle c = detail_curve::unwrapHandle(info[0]);
  const size_t i = info[1].As<Napi::Number>().Uint32Value();
  const size_t j = info[2].As<Napi::Number>().Uint32Value();
  const std::string amt = detail_curve::bigintToDecimal(env, info[3].As<Napi::BigInt>());
  return detail_curve::decimalToBigint(
      env, detail_curve::opOut(env, &flox_curve_amount_out, c, i, j, amt).c_str());
}

inline Napi::Value ammCurveApplySwap(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  FloxCurveHandle c = detail_curve::unwrapHandle(info[0]);
  const size_t i = info[1].As<Napi::Number>().Uint32Value();
  const size_t j = info[2].As<Napi::Number>().Uint32Value();
  const std::string amt = detail_curve::bigintToDecimal(env, info[3].As<Napi::BigInt>());
  return detail_curve::decimalToBigint(
      env, detail_curve::opOut(env, &flox_curve_apply_swap, c, i, j, amt).c_str());
}

inline Napi::Value ammCurveBalance(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  FloxCurveHandle c = detail_curve::unwrapHandle(info[0]);
  const size_t i = info[1].As<Napi::Number>().Uint32Value();
  char out[96] = {0};
  if (!flox_curve_balance(c, i, out, sizeof(out)))
  {
    throw Napi::RangeError::New(env, "amm curve: balance index out of range");
  }
  return detail_curve::decimalToBigint(env, out);
}

// A concentrated-liquidity pool's sqrt price / active liquidity as a BigInt, or null
// for a non-CLMM pool (constant-product, Raydium CP).
inline Napi::Value ammCurveClmmField(const Napi::CallbackInfo& info,
                                     uint8_t (*op)(FloxCurveHandle, char*, size_t))
{
  Napi::Env env = info.Env();
  char out[96] = {0};
  if (!op(detail_curve::unwrapHandle(info[0]), out, sizeof(out)))
  {
    return env.Null();
  }
  return detail_curve::decimalToBigint(env, out);
}

inline Napi::Value ammCurveSqrtPrice(const Napi::CallbackInfo& info)
{
  return ammCurveClmmField(info, &flox_curve_sqrt_price);
}

inline Napi::Value ammCurveLiquidity(const Napi::CallbackInfo& info)
{
  return ammCurveClmmField(info, &flox_curve_liquidity);
}

inline Napi::Value ammCurveClone(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  return detail_curve::wrapHandle(env, flox_curve_clone(detail_curve::unwrapHandle(info[0])));
}

inline void registerAmmCurve(Napi::Env env, Napi::Object exports)
{
  exports.Set("ammCurveConstantProduct",
              Napi::Function::New(env, &ammCurveConstantProduct, "ammCurveConstantProduct"));
  exports.Set("ammCurveRaydiumCp",
              Napi::Function::New(env, &ammCurveRaydiumCp, "ammCurveRaydiumCp"));
  exports.Set("ammCurveUniswapV3",
              Napi::Function::New(env, &ammCurveUniswapV3, "ammCurveUniswapV3"));
  exports.Set("ammCurveTokenCount",
              Napi::Function::New(env, &ammCurveTokenCount, "ammCurveTokenCount"));
  exports.Set("ammCurveAmountOut",
              Napi::Function::New(env, &ammCurveAmountOut, "ammCurveAmountOut"));
  exports.Set("ammCurveApplySwap",
              Napi::Function::New(env, &ammCurveApplySwap, "ammCurveApplySwap"));
  exports.Set("ammCurveBalance", Napi::Function::New(env, &ammCurveBalance, "ammCurveBalance"));
  exports.Set("ammCurveSqrtPrice",
              Napi::Function::New(env, &ammCurveSqrtPrice, "ammCurveSqrtPrice"));
  exports.Set("ammCurveLiquidity",
              Napi::Function::New(env, &ammCurveLiquidity, "ammCurveLiquidity"));
  exports.Set("ammCurveClone", Napi::Function::New(env, &ammCurveClone, "ammCurveClone"));
}

}  // namespace node_flox
