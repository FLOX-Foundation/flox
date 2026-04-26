#include "js_bindings.h"
#include "flox/capi/flox_capi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace flox
{

// ============================================================
// Opaque handle class — safe way to pass FloxStrategyHandle through JS
// ============================================================

static JSClassID jsHandleClassId = 0;

static JSClassDef jsHandleClassDef = {
    .class_name = "FloxHandle",
    .finalizer = nullptr,
};

JSValue createHandleObject(JSContext* ctx, void* handle)
{
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(jsHandleClassId));
  JS_SetOpaque(obj, handle);
  return obj;
}

void* getHandlePtr(JSContext* ctx, JSValueConst val)
{
  return JS_GetOpaque(val, jsHandleClassId);
}

// ============================================================
// Helpers
// ============================================================

static void* getHandle(JSContext* ctx, JSValueConst argv0)
{
  return getHandlePtr(ctx, argv0);
}

static double toDouble(JSContext* ctx, JSValueConst val)
{
  double d = 0;
  JS_ToFloat64(ctx, &d, val);
  return d;
}

static uint32_t toUint32(JSContext* ctx, JSValueConst val)
{
  uint32_t u = 0;
  JS_ToUint32(ctx, &u, val);
  return u;
}

static int64_t toInt64(JSContext* ctx, JSValueConst val)
{
  int64_t i = 0;
  JS_ToInt64(ctx, &i, val);
  return i;
}

#define GET_HANDLE_OR_THROW(ctx, argv)                                                    \
  auto h = static_cast<FloxStrategyHandle>(getHandle((ctx), (argv)[0]));                  \
  if (!h)                                                                                 \
  {                                                                                       \
    return JS_ThrowTypeError((ctx), "Strategy handle is null (not connected to engine)"); \
  }

// ============================================================
// Signal emission bindings
// ============================================================

static JSValue js_emit_market_buy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  double qty = toDouble(ctx, argv[2]);
  uint64_t oid = flox_emit_market_buy(h, sym, flox_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_market_sell(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  double qty = toDouble(ctx, argv[2]);
  uint64_t oid = flox_emit_market_sell(h, sym, flox_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_limit_buy_tif(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  double price = toDouble(ctx, argv[2]);
  double qty = toDouble(ctx, argv[3]);
  uint32_t tif = toUint32(ctx, argv[4]);
  uint64_t oid = flox_emit_limit_buy_tif(h, sym, flox_price_from_double(price),
                                         flox_quantity_from_double(qty),
                                         static_cast<uint8_t>(tif));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_limit_sell_tif(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  double price = toDouble(ctx, argv[2]);
  double qty = toDouble(ctx, argv[3]);
  uint32_t tif = toUint32(ctx, argv[4]);
  uint64_t oid = flox_emit_limit_sell_tif(h, sym, flox_price_from_double(price),
                                          flox_quantity_from_double(qty),
                                          static_cast<uint8_t>(tif));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_cancel(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  int64_t oid = toInt64(ctx, argv[1]);
  flox_emit_cancel(h, static_cast<uint64_t>(oid));
  return JS_UNDEFINED;
}

static JSValue js_emit_cancel_all(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  flox_emit_cancel_all(h, sym);
  return JS_UNDEFINED;
}

static JSValue js_emit_modify(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  int64_t oid = toInt64(ctx, argv[1]);
  double price = toDouble(ctx, argv[2]);
  double qty = toDouble(ctx, argv[3]);
  flox_emit_modify(h, static_cast<uint64_t>(oid), flox_price_from_double(price),
                   flox_quantity_from_double(qty));
  return JS_UNDEFINED;
}

static JSValue js_emit_stop_market(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  double trigger = toDouble(ctx, argv[3]);
  double qty = toDouble(ctx, argv[4]);
  uint64_t oid = flox_emit_stop_market(h, sym, static_cast<uint8_t>(side),
                                       flox_price_from_double(trigger),
                                       flox_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_stop_limit(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  double trigger = toDouble(ctx, argv[3]);
  double limit = toDouble(ctx, argv[4]);
  double qty = toDouble(ctx, argv[5]);
  uint64_t oid = flox_emit_stop_limit(h, sym, static_cast<uint8_t>(side),
                                      flox_price_from_double(trigger),
                                      flox_price_from_double(limit),
                                      flox_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_take_profit_market(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  double trigger = toDouble(ctx, argv[3]);
  double qty = toDouble(ctx, argv[4]);
  uint64_t oid = flox_emit_take_profit_market(h, sym, static_cast<uint8_t>(side),
                                              flox_price_from_double(trigger),
                                              flox_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_take_profit_limit(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  double trigger = toDouble(ctx, argv[3]);
  double limit = toDouble(ctx, argv[4]);
  double qty = toDouble(ctx, argv[5]);
  uint64_t oid = flox_emit_take_profit_limit(h, sym, static_cast<uint8_t>(side),
                                             flox_price_from_double(trigger),
                                             flox_price_from_double(limit),
                                             flox_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_trailing_stop(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  double offset = toDouble(ctx, argv[3]);
  double qty = toDouble(ctx, argv[4]);
  uint64_t oid = flox_emit_trailing_stop(h, sym, static_cast<uint8_t>(side),
                                         flox_price_from_double(offset),
                                         flox_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_trailing_stop_pct(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  int32_t bps = static_cast<int32_t>(toInt64(ctx, argv[3]));
  double qty = toDouble(ctx, argv[4]);
  uint64_t oid = flox_emit_trailing_stop_percent(h, sym, static_cast<uint8_t>(side), bps,
                                                 flox_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_close_position(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint64_t oid = flox_emit_close_position(h, sym);
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

// ============================================================
// Context query bindings
// ============================================================

static JSValue js_position(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = flox_position_raw(h, sym);
  return JS_NewFloat64(ctx, flox_quantity_to_double(raw));
}

static JSValue js_last_trade_price(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = flox_last_trade_price_raw(h, sym);
  return JS_NewFloat64(ctx, flox_price_to_double(raw));
}

static JSValue js_best_bid(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = flox_best_bid_raw(h, sym);
  return JS_NewFloat64(ctx, flox_price_to_double(raw));
}

static JSValue js_best_ask(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = flox_best_ask_raw(h, sym);
  return JS_NewFloat64(ctx, flox_price_to_double(raw));
}

static JSValue js_mid_price(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = flox_mid_price_raw(h, sym);
  return JS_NewFloat64(ctx, flox_price_to_double(raw));
}

static JSValue js_get_order_status(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  int64_t oid = toInt64(ctx, argv[1]);
  int32_t status = flox_get_order_status(h, static_cast<uint64_t>(oid));
  return JS_NewInt32(ctx, status);
}

// ============================================================
// Batch indicators
// ============================================================

static JSValue jsArrayFromDoubles(JSContext* ctx, const std::vector<double>& data)
{
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < data.size(); i++)
  {
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), JS_NewFloat64(ctx, data[i]));
  }
  return arr;
}

static std::vector<double> jsArrayToDoubles(JSContext* ctx, JSValueConst arr)
{
  JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
  uint32_t len = 0;
  JS_ToUint32(ctx, &len, lenVal);
  JS_FreeValue(ctx, lenVal);

  std::vector<double> result(len);
  for (uint32_t i = 0; i < len; i++)
  {
    JSValue elem = JS_GetPropertyUint32(ctx, arr, i);
    JS_ToFloat64(ctx, &result[i], elem);
    JS_FreeValue(ctx, elem);
  }
  return result;
}

static JSValue js_indicator_sma(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  flox_indicator_sma(data.data(), data.size(), period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_ema(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  flox_indicator_ema(data.data(), data.size(), period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_rsi(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  flox_indicator_rsi(data.data(), data.size(), period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_atr(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  uint32_t period = toUint32(ctx, argv[3]);
  size_t len = high.size();
  std::vector<double> output(len);
  flox_indicator_atr(high.data(), low.data(), close.data(), len, period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_macd(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t fast = (argc > 1) ? toUint32(ctx, argv[1]) : 12;
  uint32_t slow = (argc > 2) ? toUint32(ctx, argv[2]) : 26;
  uint32_t signal = (argc > 3) ? toUint32(ctx, argv[3]) : 9;
  size_t len = data.size();
  std::vector<double> macdOut(len), signalOut(len), histOut(len);
  flox_indicator_macd(data.data(), len, fast, slow, signal, macdOut.data(), signalOut.data(),
                      histOut.data());
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "line", jsArrayFromDoubles(ctx, macdOut));
  JS_SetPropertyStr(ctx, result, "signal", jsArrayFromDoubles(ctx, signalOut));
  JS_SetPropertyStr(ctx, result, "histogram", jsArrayFromDoubles(ctx, histOut));
  return result;
}

static JSValue js_indicator_bollinger(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = (argc > 1) ? toUint32(ctx, argv[1]) : 20;
  double multiplier = (argc > 2) ? toDouble(ctx, argv[2]) : 2.0;
  size_t len = data.size();
  std::vector<double> upper(len), middle(len), lower(len);
  flox_indicator_bollinger(data.data(), len, period, multiplier, upper.data(), middle.data(),
                           lower.data());
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "upper", jsArrayFromDoubles(ctx, upper));
  JS_SetPropertyStr(ctx, result, "middle", jsArrayFromDoubles(ctx, middle));
  JS_SetPropertyStr(ctx, result, "lower", jsArrayFromDoubles(ctx, lower));
  return result;
}

static JSValue js_indicator_rma(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  flox_indicator_rma(data.data(), data.size(), period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_dema(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  flox_indicator_dema(data.data(), data.size(), period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_tema(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  flox_indicator_tema(data.data(), data.size(), period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_kama(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  uint32_t fast = (argc > 2) ? toUint32(ctx, argv[2]) : 2;
  uint32_t slow = (argc > 3) ? toUint32(ctx, argv[3]) : 30;
  std::vector<double> output(data.size());
  flox_indicator_kama(data.data(), data.size(), period, fast, slow, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_slope(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t length = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  flox_indicator_slope(data.data(), data.size(), length, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_adx(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  uint32_t period = toUint32(ctx, argv[3]);
  size_t len = high.size();
  std::vector<double> adxOut(len), plusDi(len), minusDi(len);
  flox_indicator_adx(high.data(), low.data(), close.data(), len, period, adxOut.data(),
                     plusDi.data(), minusDi.data());
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "adx", jsArrayFromDoubles(ctx, adxOut));
  JS_SetPropertyStr(ctx, result, "plusDi", jsArrayFromDoubles(ctx, plusDi));
  JS_SetPropertyStr(ctx, result, "minusDi", jsArrayFromDoubles(ctx, minusDi));
  return result;
}

static JSValue js_indicator_cci(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  uint32_t period = toUint32(ctx, argv[3]);
  size_t len = high.size();
  std::vector<double> output(len);
  flox_indicator_cci(high.data(), low.data(), close.data(), len, period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_stochastic(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  uint32_t kPeriod = (argc > 3) ? toUint32(ctx, argv[3]) : 14;
  uint32_t dPeriod = (argc > 4) ? toUint32(ctx, argv[4]) : 3;
  size_t len = high.size();
  std::vector<double> kOut(len), dOut(len);
  flox_indicator_stochastic(high.data(), low.data(), close.data(), len, kPeriod, dPeriod,
                            kOut.data(), dOut.data());
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "k", jsArrayFromDoubles(ctx, kOut));
  JS_SetPropertyStr(ctx, result, "d", jsArrayFromDoubles(ctx, dOut));
  return result;
}

static JSValue js_indicator_chop(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  uint32_t period = toUint32(ctx, argv[3]);
  size_t len = high.size();
  std::vector<double> output(len);
  flox_indicator_chop(high.data(), low.data(), close.data(), len, period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_obv(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto close = jsArrayToDoubles(ctx, argv[0]);
  auto volume = jsArrayToDoubles(ctx, argv[1]);
  size_t len = close.size();
  std::vector<double> output(len);
  flox_indicator_obv(close.data(), volume.data(), len, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_vwap(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto close = jsArrayToDoubles(ctx, argv[0]);
  auto volume = jsArrayToDoubles(ctx, argv[1]);
  uint32_t window = toUint32(ctx, argv[2]);
  size_t len = close.size();
  std::vector<double> output(len);
  flox_indicator_vwap(close.data(), volume.data(), len, window, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_cvd(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto open = jsArrayToDoubles(ctx, argv[0]);
  auto high = jsArrayToDoubles(ctx, argv[1]);
  auto low = jsArrayToDoubles(ctx, argv[2]);
  auto close = jsArrayToDoubles(ctx, argv[3]);
  auto volume = jsArrayToDoubles(ctx, argv[4]);
  size_t len = open.size();
  std::vector<double> output(len);
  flox_indicator_cvd(open.data(), high.data(), low.data(), close.data(), volume.data(), len,
                     output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_skewness(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  flox_indicator_skewness(data.data(), data.size(), period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_kurtosis(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  flox_indicator_kurtosis(data.data(), data.size(), period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_rolling_zscore(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  flox_indicator_rolling_zscore(data.data(), data.size(), period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_shannon_entropy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  uint32_t bins = toUint32(ctx, argv[2]);
  std::vector<double> output(data.size());
  flox_indicator_shannon_entropy(data.data(), data.size(), period, bins, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_parkinson_vol(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  uint32_t period = toUint32(ctx, argv[2]);
  size_t len = high.size();
  std::vector<double> output(len);
  flox_indicator_parkinson_vol(high.data(), low.data(), len, period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_rogers_satchell_vol(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  auto open = jsArrayToDoubles(ctx, argv[0]);
  auto high = jsArrayToDoubles(ctx, argv[1]);
  auto low = jsArrayToDoubles(ctx, argv[2]);
  auto close = jsArrayToDoubles(ctx, argv[3]);
  uint32_t period = toUint32(ctx, argv[4]);
  size_t len = open.size();
  std::vector<double> output(len);
  flox_indicator_rogers_satchell_vol(open.data(), high.data(), low.data(), close.data(), len,
                                     period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_correlation(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto x = jsArrayToDoubles(ctx, argv[0]);
  auto y = jsArrayToDoubles(ctx, argv[1]);
  uint32_t period = toUint32(ctx, argv[2]);
  size_t len = x.size();
  std::vector<double> output(len);
  flox_indicator_correlation(x.data(), y.data(), len, period, output.data());
  return jsArrayFromDoubles(ctx, output);
}

// ============================================================
// Order book bindings
// ============================================================

static JSValue js_book_create(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return createHandleObject(ctx, flox_book_create(toDouble(ctx, argv[0])));
}
static JSValue js_book_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_book_destroy(static_cast<FloxBookHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_book_best_bid(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double p = 0;
  if (flox_book_best_bid(static_cast<FloxBookHandle>(getHandle(ctx, argv[0])), &p))
  {
    return JS_NewFloat64(ctx, p);
  }
  return JS_NULL;
}
static JSValue js_book_best_ask(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double p = 0;
  if (flox_book_best_ask(static_cast<FloxBookHandle>(getHandle(ctx, argv[0])), &p))
  {
    return JS_NewFloat64(ctx, p);
  }
  return JS_NULL;
}
static JSValue js_book_mid(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double p = 0;
  if (flox_book_mid(static_cast<FloxBookHandle>(getHandle(ctx, argv[0])), &p))
  {
    return JS_NewFloat64(ctx, p);
  }
  return JS_NULL;
}
static JSValue js_book_spread(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double s = 0;
  if (flox_book_spread(static_cast<FloxBookHandle>(getHandle(ctx, argv[0])), &s))
  {
    return JS_NewFloat64(ctx, s);
  }
  return JS_NULL;
}
static JSValue js_book_clear(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_book_clear(static_cast<FloxBookHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_book_is_crossed(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewBool(
      ctx, flox_book_is_crossed(static_cast<FloxBookHandle>(getHandle(ctx, argv[0]))) != 0);
}
static JSValue js_book_apply_snapshot(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxBookHandle>(getHandle(ctx, argv[0]));
  auto bp = jsArrayToDoubles(ctx, argv[1]);
  auto bq = jsArrayToDoubles(ctx, argv[2]);
  auto ap = jsArrayToDoubles(ctx, argv[3]);
  auto aq = jsArrayToDoubles(ctx, argv[4]);
  flox_book_apply_snapshot(h, bp.data(), bq.data(), bp.size(), ap.data(), aq.data(), ap.size());
  return JS_UNDEFINED;
}
static JSValue js_book_apply_delta(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxBookHandle>(getHandle(ctx, argv[0]));
  auto bp = jsArrayToDoubles(ctx, argv[1]);
  auto bq = jsArrayToDoubles(ctx, argv[2]);
  auto ap = jsArrayToDoubles(ctx, argv[3]);
  auto aq = jsArrayToDoubles(ctx, argv[4]);
  flox_book_apply_delta(h, bp.data(), bq.data(), bp.size(), ap.data(), aq.data(), ap.size());
  return JS_UNDEFINED;
}

// ============================================================
// Executor bindings
// ============================================================

static JSValue js_executor_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, flox_executor_create());
}
static JSValue js_executor_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_executor_destroy(static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_executor_submit(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0]));
  flox_executor_submit_order(h, static_cast<uint64_t>(toInt64(ctx, argv[1])),
                             static_cast<uint8_t>(toUint32(ctx, argv[2])),
                             toDouble(ctx, argv[3]), toDouble(ctx, argv[4]),
                             static_cast<uint8_t>(toUint32(ctx, argv[5])),
                             toUint32(ctx, argv[6]));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_bar(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_executor_on_bar(static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0])),
                       toUint32(ctx, argv[1]), toDouble(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_trade(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_executor_on_trade(static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0])),
                         toUint32(ctx, argv[1]), toDouble(ctx, argv[2]),
                         static_cast<uint8_t>(toUint32(ctx, argv[3])));
  return JS_UNDEFINED;
}
static JSValue js_executor_advance(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_executor_advance_clock(static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0])),
                              toInt64(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_executor_fill_count(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewUint32(
      ctx, flox_executor_fill_count(static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0]))));
}

static JSValue js_executor_set_default_slippage(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  flox_executor_set_default_slippage(
      static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<int32_t>(toInt64(ctx, argv[1])),
      static_cast<int32_t>(toInt64(ctx, argv[2])), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), toDouble(ctx, argv[5]));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_symbol_slippage(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  flox_executor_set_symbol_slippage(
      static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0])), toUint32(ctx, argv[1]),
      static_cast<int32_t>(toInt64(ctx, argv[2])),
      static_cast<int32_t>(toInt64(ctx, argv[3])), toDouble(ctx, argv[4]),
      toDouble(ctx, argv[5]), toDouble(ctx, argv[6]));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_queue_model(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  flox_executor_set_queue_model(static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0])),
                                static_cast<int32_t>(toInt64(ctx, argv[1])),
                                toUint32(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_trade_qty(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  flox_executor_on_trade_qty(static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0])),
                             toUint32(ctx, argv[1]), toDouble(ctx, argv[2]),
                             toDouble(ctx, argv[3]),
                             static_cast<uint8_t>(toUint32(ctx, argv[4])));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_best_levels(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  flox_executor_on_best_levels(static_cast<FloxExecutorHandle>(getHandle(ctx, argv[0])),
                               toUint32(ctx, argv[1]), toDouble(ctx, argv[2]),
                               toDouble(ctx, argv[3]), toDouble(ctx, argv[4]),
                               toDouble(ctx, argv[5]));
  return JS_UNDEFINED;
}

static JSValue js_backtest_result_create(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  return createHandleObject(
      ctx, flox_backtest_result_create(
               toDouble(ctx, argv[0]), toDouble(ctx, argv[1]),
               static_cast<uint8_t>(toUint32(ctx, argv[2])), toDouble(ctx, argv[3]),
               toDouble(ctx, argv[4]), toDouble(ctx, argv[5])));
}
static JSValue js_backtest_result_destroy(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  flox_backtest_result_destroy(
      static_cast<FloxBacktestResultHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_backtest_result_record_fill(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  flox_backtest_result_record_fill(
      static_cast<FloxBacktestResultHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])), toUint32(ctx, argv[2]),
      static_cast<uint8_t>(toUint32(ctx, argv[3])), toDouble(ctx, argv[4]),
      toDouble(ctx, argv[5]), toInt64(ctx, argv[6]));
  return JS_UNDEFINED;
}
static JSValue js_backtest_result_ingest(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  flox_backtest_result_ingest_executor(
      static_cast<FloxBacktestResultHandle>(getHandle(ctx, argv[0])),
      static_cast<FloxExecutorHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_backtest_result_stats(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  FloxBacktestStats s{};
  flox_backtest_result_stats(
      static_cast<FloxBacktestResultHandle>(getHandle(ctx, argv[0])), &s);
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "totalTrades", JS_NewInt64(ctx, (int64_t)s.totalTrades));
  JS_SetPropertyStr(ctx, obj, "winningTrades", JS_NewInt64(ctx, (int64_t)s.winningTrades));
  JS_SetPropertyStr(ctx, obj, "losingTrades", JS_NewInt64(ctx, (int64_t)s.losingTrades));
  JS_SetPropertyStr(ctx, obj, "maxConsecutiveWins",
                    JS_NewInt64(ctx, (int64_t)s.maxConsecutiveWins));
  JS_SetPropertyStr(ctx, obj, "maxConsecutiveLosses",
                    JS_NewInt64(ctx, (int64_t)s.maxConsecutiveLosses));
  JS_SetPropertyStr(ctx, obj, "initialCapital", JS_NewFloat64(ctx, s.initialCapital));
  JS_SetPropertyStr(ctx, obj, "finalCapital", JS_NewFloat64(ctx, s.finalCapital));
  JS_SetPropertyStr(ctx, obj, "totalPnl", JS_NewFloat64(ctx, s.totalPnl));
  JS_SetPropertyStr(ctx, obj, "totalFees", JS_NewFloat64(ctx, s.totalFees));
  JS_SetPropertyStr(ctx, obj, "netPnl", JS_NewFloat64(ctx, s.netPnl));
  JS_SetPropertyStr(ctx, obj, "grossProfit", JS_NewFloat64(ctx, s.grossProfit));
  JS_SetPropertyStr(ctx, obj, "grossLoss", JS_NewFloat64(ctx, s.grossLoss));
  JS_SetPropertyStr(ctx, obj, "maxDrawdown", JS_NewFloat64(ctx, s.maxDrawdown));
  JS_SetPropertyStr(ctx, obj, "maxDrawdownPct", JS_NewFloat64(ctx, s.maxDrawdownPct));
  JS_SetPropertyStr(ctx, obj, "winRate", JS_NewFloat64(ctx, s.winRate));
  JS_SetPropertyStr(ctx, obj, "profitFactor", JS_NewFloat64(ctx, s.profitFactor));
  JS_SetPropertyStr(ctx, obj, "avgWin", JS_NewFloat64(ctx, s.avgWin));
  JS_SetPropertyStr(ctx, obj, "avgLoss", JS_NewFloat64(ctx, s.avgLoss));
  JS_SetPropertyStr(ctx, obj, "avgWinLossRatio", JS_NewFloat64(ctx, s.avgWinLossRatio));
  JS_SetPropertyStr(ctx, obj, "avgTradeDurationNs",
                    JS_NewFloat64(ctx, s.avgTradeDurationNs));
  JS_SetPropertyStr(ctx, obj, "medianTradeDurationNs",
                    JS_NewFloat64(ctx, s.medianTradeDurationNs));
  JS_SetPropertyStr(ctx, obj, "maxTradeDurationNs",
                    JS_NewFloat64(ctx, s.maxTradeDurationNs));
  JS_SetPropertyStr(ctx, obj, "sharpeRatio", JS_NewFloat64(ctx, s.sharpeRatio));
  JS_SetPropertyStr(ctx, obj, "sortinoRatio", JS_NewFloat64(ctx, s.sortinoRatio));
  JS_SetPropertyStr(ctx, obj, "calmarRatio", JS_NewFloat64(ctx, s.calmarRatio));
  JS_SetPropertyStr(ctx, obj, "timeWeightedReturn",
                    JS_NewFloat64(ctx, s.timeWeightedReturn));
  JS_SetPropertyStr(ctx, obj, "returnPct", JS_NewFloat64(ctx, s.returnPct));
  JS_SetPropertyStr(ctx, obj, "startTimeNs", JS_NewInt64(ctx, s.startTimeNs));
  JS_SetPropertyStr(ctx, obj, "endTimeNs", JS_NewInt64(ctx, s.endTimeNs));
  return obj;
}
static JSValue js_backtest_result_equity_curve(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  auto h = static_cast<FloxBacktestResultHandle>(getHandle(ctx, argv[0]));
  const uint32_t n = flox_backtest_result_equity_curve(h, nullptr, 0);
  std::vector<FloxEquityPoint> pts(n);
  if (n > 0)
  {
    flox_backtest_result_equity_curve(h, pts.data(), n);
  }
  JSValue arr = JS_NewArray(ctx);
  for (uint32_t i = 0; i < n; ++i)
  {
    JSValue pt = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, pt, "timestampNs", JS_NewInt64(ctx, pts[i].timestamp_ns));
    JS_SetPropertyStr(ctx, pt, "equity", JS_NewFloat64(ctx, pts[i].equity));
    JS_SetPropertyStr(ctx, pt, "drawdownPct", JS_NewFloat64(ctx, pts[i].drawdown_pct));
    JS_SetPropertyUint32(ctx, arr, i, pt);
  }
  return arr;
}
static JSValue js_backtest_result_write_csv(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  const char* path = JS_ToCString(ctx, argv[1]);
  if (!path)
  {
    return JS_NewBool(ctx, 0);
  }
  uint8_t ok = flox_backtest_result_write_equity_curve_csv(
      static_cast<FloxBacktestResultHandle>(getHandle(ctx, argv[0])), path);
  JS_FreeCString(ctx, path);
  return JS_NewBool(ctx, ok != 0);
}

// ============================================================
// Position tracker bindings
// ============================================================

static JSValue js_pos_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  uint8_t basis = (argc > 0) ? static_cast<uint8_t>(toUint32(ctx, argv[0])) : 0;
  return createHandleObject(ctx, flox_position_tracker_create(basis));
}
static JSValue js_pos_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_position_tracker_destroy(
      static_cast<FloxPositionTrackerHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_pos_on_fill(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_position_tracker_on_fill(
      static_cast<FloxPositionTrackerHandle>(getHandle(ctx, argv[0])), toUint32(ctx, argv[1]),
      static_cast<uint8_t>(toUint32(ctx, argv[2])), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]));
  return JS_UNDEFINED;
}
static JSValue js_pos_position(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(ctx, flox_position_tracker_position(
                                static_cast<FloxPositionTrackerHandle>(getHandle(ctx, argv[0])),
                                toUint32(ctx, argv[1])));
}
static JSValue js_pos_avg_entry(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(ctx, flox_position_tracker_avg_entry(
                                static_cast<FloxPositionTrackerHandle>(getHandle(ctx, argv[0])),
                                toUint32(ctx, argv[1])));
}
static JSValue js_pos_pnl(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, flox_position_tracker_realized_pnl(
               static_cast<FloxPositionTrackerHandle>(getHandle(ctx, argv[0])),
               toUint32(ctx, argv[1])));
}
static JSValue js_pos_total_pnl(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(ctx, flox_position_tracker_total_pnl(
                                static_cast<FloxPositionTrackerHandle>(getHandle(ctx, argv[0]))));
}

// ============================================================
// Volume profile bindings
// ============================================================

static JSValue js_vprofile_create(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return createHandleObject(ctx, flox_volume_profile_create(toDouble(ctx, argv[0])));
}
static JSValue js_vprofile_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_volume_profile_destroy(static_cast<FloxVolumeProfileHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_vprofile_add_trade(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_volume_profile_add_trade(
      static_cast<FloxVolumeProfileHandle>(getHandle(ctx, argv[0])), toDouble(ctx, argv[1]),
      toDouble(ctx, argv[2]), static_cast<uint8_t>(toUint32(ctx, argv[3])));
  return JS_UNDEFINED;
}
static JSValue js_vprofile_poc(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, flox_volume_profile_poc(static_cast<FloxVolumeProfileHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_vprofile_vah(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, flox_volume_profile_vah(static_cast<FloxVolumeProfileHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_vprofile_val(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, flox_volume_profile_val(static_cast<FloxVolumeProfileHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_vprofile_total_vol(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(ctx, flox_volume_profile_total_volume(
                                static_cast<FloxVolumeProfileHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_vprofile_clear(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_volume_profile_clear(static_cast<FloxVolumeProfileHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

// ============================================================
// Stats
// ============================================================

static JSValue js_stat_correlation(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto x = jsArrayToDoubles(ctx, argv[0]);
  auto y = jsArrayToDoubles(ctx, argv[1]);
  return JS_NewFloat64(ctx, flox_stat_correlation(x.data(), y.data(),
                                                  std::min(x.size(), y.size())));
}

// ============================================================
// Console shim
// ============================================================

static JSValue js_console_impl(JSContext* ctx, int argc, JSValueConst* argv, FILE* stream)
{
  for (int i = 0; i < argc; i++)
  {
    if (i > 0)
    {
      fputc(' ', stream);
    }
    if (JS_IsObject(argv[i]) && !JS_IsFunction(ctx, argv[i]))
    {
      JSValue json = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);
      const char* s = JS_ToCString(ctx, json);
      if (s)
      {
        fputs(s, stream);
        JS_FreeCString(ctx, s);
      }
      JS_FreeValue(ctx, json);
    }
    else
    {
      const char* str = JS_ToCString(ctx, argv[i]);
      if (str)
      {
        fputs(str, stream);
        JS_FreeCString(ctx, str);
      }
    }
  }
  fputc('\n', stream);
  return JS_UNDEFINED;
}

static JSValue js_console_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  return js_console_impl(ctx, argc, argv, stdout);
}

static JSValue js_console_warn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  fputs("[warn] ", stderr);
  return js_console_impl(ctx, argc, argv, stderr);
}

static JSValue js_console_error(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  fputs("[error] ", stderr);
  return js_console_impl(ctx, argc, argv, stderr);
}

// ============================================================
// Registration
// ============================================================

static void addGlobalFunc(JSContext* ctx, const char* name, JSCFunction* func, int argc)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, name, JS_NewCFunction(ctx, func, name, argc));
  JS_FreeValue(ctx, global);
}

// ============================================================
// MarketProfile
// ============================================================

static JSValue js_mp_create(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return createHandleObject(c, flox_market_profile_create(toDouble(c, a[0]), toUint32(c, a[1]), toInt64(c, a[2])));
}

static JSValue js_mp_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_market_profile_destroy(static_cast<FloxMarketProfileHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_mp_add_trade(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_market_profile_add_trade(static_cast<FloxMarketProfileHandle>(getHandle(c, a[0])),
                                toInt64(c, a[1]), toDouble(c, a[2]), toDouble(c, a[3]), static_cast<uint8_t>(toUint32(c, a[4])));
  return JS_UNDEFINED;
}

static JSValue js_mp_poc(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, flox_market_profile_poc(static_cast<FloxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_vah(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, flox_market_profile_vah(static_cast<FloxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_val(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, flox_market_profile_val(static_cast<FloxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_ib_high(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, flox_market_profile_ib_high(static_cast<FloxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_ib_low(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, flox_market_profile_ib_low(static_cast<FloxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_is_poor_high(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, flox_market_profile_is_poor_high(static_cast<FloxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_is_poor_low(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, flox_market_profile_is_poor_low(static_cast<FloxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_clear(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_market_profile_clear(static_cast<FloxMarketProfileHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

// ============================================================
// CompositeBook
// ============================================================

static JSValue js_cb_create(JSContext* c, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(c, flox_composite_book_create());
}

static JSValue js_cb_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_composite_book_destroy(static_cast<FloxCompositeBookHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_cb_best_bid(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  double p = 0, q = 0;
  if (flox_composite_book_best_bid(static_cast<FloxCompositeBookHandle>(getHandle(c, a[0])),
                                   toUint32(c, a[1]), &p, &q))
  {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "price", JS_NewFloat64(c, p));
    JS_SetPropertyStr(c, o, "qty", JS_NewFloat64(c, q));
    return o;
  }
  return JS_NULL;
}

static JSValue js_cb_best_ask(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  double p = 0, q = 0;
  if (flox_composite_book_best_ask(static_cast<FloxCompositeBookHandle>(getHandle(c, a[0])),
                                   toUint32(c, a[1]), &p, &q))
  {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "price", JS_NewFloat64(c, p));
    JS_SetPropertyStr(c, o, "qty", JS_NewFloat64(c, q));
    return o;
  }
  return JS_NULL;
}

static JSValue js_cb_has_arb(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, flox_composite_book_has_arb(static_cast<FloxCompositeBookHandle>(getHandle(c, a[0])), toUint32(c, a[1])));
}

static JSValue js_cb_mark_stale(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_composite_book_mark_stale(static_cast<FloxCompositeBookHandle>(getHandle(c, a[0])), toUint32(c, a[1]), toUint32(c, a[2]));
  return JS_UNDEFINED;
}

static JSValue js_cb_check_staleness(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_composite_book_check_staleness(static_cast<FloxCompositeBookHandle>(getHandle(c, a[0])), toInt64(c, a[1]), toInt64(c, a[2]));
  return JS_UNDEFINED;
}

// ============================================================
// OrderTracker
// ============================================================

static JSValue js_ot_create(JSContext* c, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(c, flox_order_tracker_create());
}

static JSValue js_ot_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_order_tracker_destroy(static_cast<FloxOrderTrackerHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_ot_submit(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, flox_order_tracker_on_submitted(static_cast<FloxOrderTrackerHandle>(getHandle(c, a[0])),
                                                       static_cast<uint64_t>(toInt64(c, a[1])), toUint32(c, a[2]), static_cast<uint8_t>(toUint32(c, a[3])),
                                                       toDouble(c, a[4]), toDouble(c, a[5])));
}

static JSValue js_ot_filled(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, flox_order_tracker_on_filled(static_cast<FloxOrderTrackerHandle>(getHandle(c, a[0])),
                                                    static_cast<uint64_t>(toInt64(c, a[1])), toDouble(c, a[2])));
}

static JSValue js_ot_canceled(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, flox_order_tracker_on_canceled(static_cast<FloxOrderTrackerHandle>(getHandle(c, a[0])),
                                                      static_cast<uint64_t>(toInt64(c, a[1]))));
}

static JSValue js_ot_is_active(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, flox_order_tracker_is_active(static_cast<FloxOrderTrackerHandle>(getHandle(c, a[0])),
                                                    static_cast<uint64_t>(toInt64(c, a[1]))));
}

static JSValue js_ot_active_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewUint32(c, flox_order_tracker_active_count(static_cast<FloxOrderTrackerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ot_total_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewUint32(c, flox_order_tracker_total_count(static_cast<FloxOrderTrackerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ot_prune(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_order_tracker_prune(static_cast<FloxOrderTrackerHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

// ============================================================
// PositionGroupTracker
// ============================================================

static JSValue js_pg_create(JSContext* c, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(c, flox_position_group_create());
}

static JSValue js_pg_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_position_group_destroy(static_cast<FloxPositionGroupHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_pg_open(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, static_cast<double>(flox_position_group_open(
                              static_cast<FloxPositionGroupHandle>(getHandle(c, a[0])),
                              static_cast<uint64_t>(toInt64(c, a[1])), toUint32(c, a[2]),
                              static_cast<uint8_t>(toUint32(c, a[3])), toDouble(c, a[4]), toDouble(c, a[5]))));
}

static JSValue js_pg_close(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_position_group_close(static_cast<FloxPositionGroupHandle>(getHandle(c, a[0])),
                            static_cast<uint64_t>(toInt64(c, a[1])), toDouble(c, a[2]));
  return JS_UNDEFINED;
}

static JSValue js_pg_partial_close(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_position_group_partial_close(static_cast<FloxPositionGroupHandle>(getHandle(c, a[0])),
                                    static_cast<uint64_t>(toInt64(c, a[1])), toDouble(c, a[2]), toDouble(c, a[3]));
  return JS_UNDEFINED;
}

static JSValue js_pg_net(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, flox_position_group_net_position(
                              static_cast<FloxPositionGroupHandle>(getHandle(c, a[0])), toUint32(c, a[1])));
}

static JSValue js_pg_pnl(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, flox_position_group_realized_pnl(
                              static_cast<FloxPositionGroupHandle>(getHandle(c, a[0])), toUint32(c, a[1])));
}

static JSValue js_pg_total_pnl(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, flox_position_group_total_pnl(
                              static_cast<FloxPositionGroupHandle>(getHandle(c, a[0]))));
}

static JSValue js_pg_open_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewUint32(c, flox_position_group_open_count(
                             static_cast<FloxPositionGroupHandle>(getHandle(c, a[0])), toUint32(c, a[1])));
}

static JSValue js_pg_prune(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_position_group_prune(static_cast<FloxPositionGroupHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

// ============================================================
// DataWriter
// ============================================================

static JSValue js_dw_create(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  const char* dir = JS_ToCString(c, a[0]);
  uint64_t mb = (JS_IsUndefined(a[1]) || JS_IsNull(a[1])) ? 256 : static_cast<uint64_t>(toInt64(c, a[1]));
  uint8_t eid = (JS_IsUndefined(a[2]) || JS_IsNull(a[2])) ? 0 : static_cast<uint8_t>(toUint32(c, a[2]));
  JSValue ret = createHandleObject(c, flox_data_writer_create(dir, mb, eid));
  JS_FreeCString(c, dir);
  return ret;
}

static JSValue js_dw_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_data_writer_destroy(static_cast<FloxDataWriterHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_dw_write_trade(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, flox_data_writer_write_trade(
                           static_cast<FloxDataWriterHandle>(getHandle(c, a[0])),
                           toInt64(c, a[1]), toInt64(c, a[2]), toDouble(c, a[3]), toDouble(c, a[4]),
                           static_cast<uint64_t>(toInt64(c, a[5])), toUint32(c, a[6]),
                           static_cast<uint8_t>(toUint32(c, a[7]))));
}

static JSValue js_dw_flush(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_data_writer_flush(static_cast<FloxDataWriterHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_dw_close(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_data_writer_close(static_cast<FloxDataWriterHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_dw_stats(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  FloxWriterStats s = flox_data_writer_stats(static_cast<FloxDataWriterHandle>(getHandle(c, a[0])));
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "bytesWritten", JS_NewInt64(c, static_cast<int64_t>(s.bytes_written)));
  JS_SetPropertyStr(c, o, "eventsWritten", JS_NewInt64(c, static_cast<int64_t>(s.events_written)));
  JS_SetPropertyStr(c, o, "segmentsCreated", JS_NewInt64(c, static_cast<int64_t>(s.segments_created)));
  JS_SetPropertyStr(c, o, "tradesWritten", JS_NewInt64(c, static_cast<int64_t>(s.trades_written)));
  return o;
}

// ============================================================
// DataReader
// ============================================================

static JSValue js_dr_create(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  const char* dir = JS_ToCString(c, a[0]);
  JSValue ret = createHandleObject(c, flox_data_reader_create(dir));
  JS_FreeCString(c, dir);
  return ret;
}

static JSValue js_dr_create_filtered(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  const char* dir = JS_ToCString(c, a[0]);
  int64_t from_ns = toInt64(c, a[1]);
  int64_t to_ns = toInt64(c, a[2]);
  // a[3] optional: array of symbol ids
  std::vector<uint32_t> syms;
  if (argc > 3 && JS_IsArray(c, a[3]))
  {
    JSValue lenVal = JS_GetPropertyStr(c, a[3], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lenVal);
    JS_FreeValue(c, lenVal);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[3], i);
      syms.push_back(toUint32(c, e));
      JS_FreeValue(c, e);
    }
  }
  JSValue ret = createHandleObject(c, flox_data_reader_create_filtered(
                                          dir, from_ns, to_ns, syms.empty() ? nullptr : syms.data(),
                                          static_cast<uint32_t>(syms.size())));
  JS_FreeCString(c, dir);
  return ret;
}

static JSValue js_dr_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_data_reader_destroy(static_cast<FloxDataReaderHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_dr_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewInt64(c, static_cast<int64_t>(flox_data_reader_count(
                            static_cast<FloxDataReaderHandle>(getHandle(c, a[0])))));
}

static JSValue js_dr_summary(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  FloxDatasetSummary s = flox_data_reader_summary(static_cast<FloxDataReaderHandle>(getHandle(c, a[0])));
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "firstEventNs", JS_NewInt64(c, s.first_event_ns));
  JS_SetPropertyStr(c, o, "lastEventNs", JS_NewInt64(c, s.last_event_ns));
  JS_SetPropertyStr(c, o, "totalEvents", JS_NewInt64(c, static_cast<int64_t>(s.total_events)));
  JS_SetPropertyStr(c, o, "segmentCount", JS_NewUint32(c, s.segment_count));
  JS_SetPropertyStr(c, o, "totalBytes", JS_NewInt64(c, static_cast<int64_t>(s.total_bytes)));
  JS_SetPropertyStr(c, o, "durationSeconds", JS_NewFloat64(c, s.duration_seconds));
  return o;
}

static JSValue js_dr_stats(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  FloxReaderStats s = flox_data_reader_stats(static_cast<FloxDataReaderHandle>(getHandle(c, a[0])));
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "filesRead", JS_NewInt64(c, static_cast<int64_t>(s.files_read)));
  JS_SetPropertyStr(c, o, "eventsRead", JS_NewInt64(c, static_cast<int64_t>(s.events_read)));
  JS_SetPropertyStr(c, o, "tradesRead", JS_NewInt64(c, static_cast<int64_t>(s.trades_read)));
  JS_SetPropertyStr(c, o, "bookUpdatesRead", JS_NewInt64(c, static_cast<int64_t>(s.book_updates_read)));
  JS_SetPropertyStr(c, o, "bytesRead", JS_NewInt64(c, static_cast<int64_t>(s.bytes_read)));
  JS_SetPropertyStr(c, o, "crcErrors", JS_NewInt64(c, static_cast<int64_t>(s.crc_errors)));
  return o;
}

static JSValue js_dr_read_trades(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<FloxDataReaderHandle>(getHandle(c, a[0]));
  uint64_t max = argc > 1 ? static_cast<uint64_t>(toInt64(c, a[1])) : 0;
  uint64_t n = flox_data_reader_count(h);
  if (max > 0 && max < n)
  {
    n = max;
  }
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxTradeRecord> trades(n);
  uint64_t got = flox_data_reader_read_trades(h, trades.data(), n);
  JSValue arr = JS_NewArray(c);
  for (uint64_t i = 0; i < got; i++)
  {
    const auto& t = trades[i];
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "exchangeTsNs", JS_NewInt64(c, t.exchange_ts_ns));
    JS_SetPropertyStr(c, o, "recvTsNs", JS_NewInt64(c, t.recv_ts_ns));
    JS_SetPropertyStr(c, o, "price", JS_NewFloat64(c, static_cast<double>(t.price_raw) / 1e8));
    JS_SetPropertyStr(c, o, "qty", JS_NewFloat64(c, static_cast<double>(t.qty_raw) / 1e8));
    JS_SetPropertyStr(c, o, "tradeId", JS_NewInt64(c, static_cast<int64_t>(t.trade_id)));
    JS_SetPropertyStr(c, o, "symbolId", JS_NewUint32(c, t.symbol_id));
    JS_SetPropertyStr(c, o, "side", JS_NewString(c, t.side == 0 ? "buy" : "sell"));
    JS_SetPropertyUint32(c, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

// ============================================================
// DataRecorder
// ============================================================

static JSValue js_recorder_create(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  const char* dir = JS_ToCString(c, a[0]);
  const char* exc = argc > 1 ? JS_ToCString(c, a[1]) : "";
  uint64_t mb = argc > 2 ? static_cast<uint64_t>(toInt64(c, a[2])) : 256;
  JSValue ret = createHandleObject(c, flox_data_recorder_create(dir, exc ? exc : "", mb));
  JS_FreeCString(c, dir);
  if (argc > 1)
  {
    JS_FreeCString(c, exc);
  }
  return ret;
}

static JSValue js_recorder_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_data_recorder_destroy(static_cast<FloxDataRecorderHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_recorder_add_symbol(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<FloxDataRecorderHandle>(getHandle(c, a[0]));
  const char* name = JS_ToCString(c, a[2]);
  const char* base = argc > 3 ? JS_ToCString(c, a[3]) : "";
  const char* quote = argc > 4 ? JS_ToCString(c, a[4]) : "";
  int8_t pp = argc > 5 ? static_cast<int8_t>(toInt64(c, a[5])) : 8;
  int8_t qp = argc > 6 ? static_cast<int8_t>(toInt64(c, a[6])) : 8;
  flox_data_recorder_add_symbol(h, toUint32(c, a[1]), name, base ? base : "", quote ? quote : "", pp, qp);
  JS_FreeCString(c, name);
  if (argc > 3)
  {
    JS_FreeCString(c, base);
  }
  if (argc > 4)
  {
    JS_FreeCString(c, quote);
  }
  return JS_UNDEFINED;
}

static JSValue js_recorder_start(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_data_recorder_start(static_cast<FloxDataRecorderHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_recorder_stop(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_data_recorder_stop(static_cast<FloxDataRecorderHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_recorder_flush(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_data_recorder_flush(static_cast<FloxDataRecorderHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_recorder_is_recording(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, flox_data_recorder_is_recording(static_cast<FloxDataRecorderHandle>(getHandle(c, a[0]))));
}

// ============================================================
// Partitioner
// ============================================================

static JSValue partitionArrayToJs(JSContext* ctx, const std::vector<FloxPartition>& parts)
{
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < parts.size(); i++)
  {
    const auto& p = parts[i];
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "partitionId", JS_NewUint32(ctx, p.partition_id));
    JS_SetPropertyStr(ctx, o, "fromNs", JS_NewInt64(ctx, p.from_ns));
    JS_SetPropertyStr(ctx, o, "toNs", JS_NewInt64(ctx, p.to_ns));
    JS_SetPropertyStr(ctx, o, "warmupFromNs", JS_NewInt64(ctx, p.warmup_from_ns));
    JS_SetPropertyStr(ctx, o, "estimatedEvents", JS_NewInt64(ctx, static_cast<int64_t>(p.estimated_events)));
    JS_SetPropertyStr(ctx, o, "estimatedBytes", JS_NewInt64(ctx, static_cast<int64_t>(p.estimated_bytes)));
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

static std::vector<FloxPartition> doPartition(
    JSContext* c, FloxPartitionerHandle h,
    uint32_t (*fn)(FloxPartitionerHandle, uint32_t, int64_t, FloxPartition*, uint32_t),
    uint32_t num, int64_t warmup)
{
  uint32_t n = fn(h, num, warmup, nullptr, 0);
  if (n == 0)
  {
    return {};
  }
  std::vector<FloxPartition> out(n);
  fn(h, num, warmup, out.data(), n);
  return out;
}

static JSValue js_part_create(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  const char* dir = JS_ToCString(c, a[0]);
  JSValue ret = createHandleObject(c, flox_partitioner_create(dir));
  JS_FreeCString(c, dir);
  return ret;
}

static JSValue js_part_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_partitioner_destroy(static_cast<FloxPartitionerHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_part_by_time(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<FloxPartitionerHandle>(getHandle(c, a[0]));
  auto parts = doPartition(c, h, flox_partitioner_by_time, toUint32(c, a[1]),
                           argc > 2 ? toInt64(c, a[2]) : 0);
  return partitionArrayToJs(c, parts);
}

static JSValue js_part_by_duration(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<FloxPartitionerHandle>(getHandle(c, a[0]));
  uint32_t n = flox_partitioner_by_duration(h, toInt64(c, a[1]),
                                            argc > 2 ? toInt64(c, a[2]) : 0, nullptr, 0);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxPartition> out(n);
  flox_partitioner_by_duration(h, toInt64(c, a[1]), argc > 2 ? toInt64(c, a[2]) : 0, out.data(), n);
  return partitionArrayToJs(c, out);
}

static JSValue js_part_by_calendar(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<FloxPartitionerHandle>(getHandle(c, a[0]));
  uint8_t unit = argc > 1 ? static_cast<uint8_t>(toUint32(c, a[1])) : 2;
  int64_t warmup = argc > 2 ? toInt64(c, a[2]) : 0;
  uint32_t n = flox_partitioner_by_calendar(h, unit, warmup, nullptr, 0);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxPartition> out(n);
  flox_partitioner_by_calendar(h, unit, warmup, out.data(), n);
  return partitionArrayToJs(c, out);
}

static JSValue js_part_by_symbol(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxPartitionerHandle>(getHandle(c, a[0]));
  uint32_t n = flox_partitioner_by_symbol(h, toUint32(c, a[1]), nullptr, 0);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxPartition> out(n);
  flox_partitioner_by_symbol(h, toUint32(c, a[1]), out.data(), n);
  return partitionArrayToJs(c, out);
}

static JSValue js_part_per_symbol(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxPartitionerHandle>(getHandle(c, a[0]));
  uint32_t n = flox_partitioner_per_symbol(h, nullptr, 0);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxPartition> out(n);
  flox_partitioner_per_symbol(h, out.data(), n);
  return partitionArrayToJs(c, out);
}

static JSValue js_part_by_event_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxPartitionerHandle>(getHandle(c, a[0]));
  uint32_t n = flox_partitioner_by_event_count(h, toUint32(c, a[1]), nullptr, 0);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxPartition> out(n);
  flox_partitioner_by_event_count(h, toUint32(c, a[1]), out.data(), n);
  return partitionArrayToJs(c, out);
}

// ============================================================
// Bar aggregators
// ============================================================

#include <algorithm>
#include <fstream>
#include <sstream>

static std::vector<int64_t> jsArrayToInt64s(JSContext* ctx, JSValueConst arr)
{
  JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
  uint32_t n = 0;
  JS_ToUint32(ctx, &n, lenVal);
  JS_FreeValue(ctx, lenVal);
  std::vector<int64_t> out(n);
  for (uint32_t i = 0; i < n; i++)
  {
    JSValue e = JS_GetPropertyUint32(ctx, arr, i);
    int64_t v = 0;
    JS_ToInt64(ctx, &v, e);
    JS_FreeValue(ctx, e);
    out[i] = v;
  }
  return out;
}

static JSValue barsToJsArray(JSContext* ctx, const std::vector<FloxBar>& bars)
{
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < bars.size(); i++)
  {
    const auto& b = bars[i];
    JSValue o = JS_NewObject(ctx);
    static constexpr double kScale = 1e8;
    JS_SetPropertyStr(ctx, o, "ts", JS_NewInt64(ctx, b.start_time_ns));
    JS_SetPropertyStr(ctx, o, "open", JS_NewFloat64(ctx, b.open_raw / kScale));
    JS_SetPropertyStr(ctx, o, "high", JS_NewFloat64(ctx, b.high_raw / kScale));
    JS_SetPropertyStr(ctx, o, "low", JS_NewFloat64(ctx, b.low_raw / kScale));
    JS_SetPropertyStr(ctx, o, "close", JS_NewFloat64(ctx, b.close_raw / kScale));
    JS_SetPropertyStr(ctx, o, "volume", JS_NewFloat64(ctx, b.volume_raw / kScale));
    JS_SetPropertyStr(ctx, o, "buyVolume", JS_NewFloat64(ctx, b.buy_volume_raw / kScale));
    JS_SetPropertyStr(ctx, o, "trades", JS_NewUint32(ctx, b.trade_count));
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

// Generic aggregator: takes (timestamps, prices, qtys, is_buy, param, maxBars)
// Returns JS array of bar objects.
using AggTimeFn = uint32_t (*)(const int64_t*, const double*, const double*, const uint8_t*,
                               size_t, double, FloxBar*, uint32_t);

static JSValue doAgg(JSContext* c, JSValueConst* a, AggTimeFn fn, double param)
{
  auto ts = jsArrayToInt64s(c, a[0]);
  auto px = jsArrayToDoubles(c, a[1]);
  auto qty = jsArrayToDoubles(c, a[2]);
  uint32_t n = static_cast<uint32_t>(ts.size());
  std::vector<uint8_t> side(n, 0);
  if (JS_IsArray(c, a[3]))
  {
    JSValue lenVal = JS_GetPropertyStr(c, a[3], "length");
    uint32_t sn = 0;
    JS_ToUint32(c, &sn, lenVal);
    JS_FreeValue(c, lenVal);
    for (uint32_t i = 0; i < std::min(sn, n); i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[3], i);
      int boolVal = JS_ToBool(c, e);
      JS_FreeValue(c, e);
      side[i] = boolVal ? 1 : 0;
    }
  }
  std::vector<FloxBar> bars(n);
  uint32_t got = fn(ts.data(), px.data(), qty.data(), side.data(), n, param, bars.data(), n);
  bars.resize(got);
  return barsToJsArray(c, bars);
}

static JSValue js_agg_time(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return doAgg(c, a, flox_aggregate_time_bars, toDouble(c, a[4]));
}

static JSValue js_agg_tick(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto ts = jsArrayToInt64s(c, a[0]);
  auto px = jsArrayToDoubles(c, a[1]);
  auto qty = jsArrayToDoubles(c, a[2]);
  uint32_t n = static_cast<uint32_t>(ts.size());
  std::vector<uint8_t> side(n, 0);
  std::vector<FloxBar> bars(n);
  uint32_t got = flox_aggregate_tick_bars(ts.data(), px.data(), qty.data(), side.data(),
                                          n, toUint32(c, a[4]), bars.data(), n);
  bars.resize(got);
  return barsToJsArray(c, bars);
}

static JSValue js_agg_volume(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return doAgg(c, a, flox_aggregate_volume_bars, toDouble(c, a[4]));
}

static JSValue js_agg_range(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return doAgg(c, a, flox_aggregate_range_bars, toDouble(c, a[4]));
}

static JSValue js_agg_renko(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return doAgg(c, a, flox_aggregate_renko_bars, toDouble(c, a[4]));
}

static JSValue js_agg_heikin(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return doAgg(c, a, flox_aggregate_heikin_ashi_bars, toDouble(c, a[4]));
}

// ============================================================
// Extended segment ops (returning JS objects)
// ============================================================

static JSValue js_seg_merge_dir(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  const char* in = JS_ToCString(c, a[0]);
  const char* out = JS_ToCString(c, a[1]);
  FloxMergeResult r = flox_segment_merge_dir(in, out);
  JS_FreeCString(c, in);
  JS_FreeCString(c, out);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "success", JS_NewBool(c, r.success));
  JS_SetPropertyStr(c, o, "segmentsMerged", JS_NewInt64(c, static_cast<int64_t>(r.segments_merged)));
  JS_SetPropertyStr(c, o, "eventsWritten", JS_NewInt64(c, static_cast<int64_t>(r.events_written)));
  JS_SetPropertyStr(c, o, "bytesWritten", JS_NewInt64(c, static_cast<int64_t>(r.bytes_written)));
  return o;
}

static JSValue js_seg_split(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  const char* in = JS_ToCString(c, a[0]);
  const char* dir = JS_ToCString(c, a[1]);
  uint8_t mode = argc > 2 ? static_cast<uint8_t>(toUint32(c, a[2])) : 0;
  int64_t tns = argc > 3 ? toInt64(c, a[3]) : 0;
  uint64_t epc = argc > 4 ? static_cast<uint64_t>(toInt64(c, a[4])) : 0;
  FloxSplitResult r = flox_segment_split(in, dir, mode, tns, epc);
  JS_FreeCString(c, in);
  JS_FreeCString(c, dir);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "success", JS_NewBool(c, r.success));
  JS_SetPropertyStr(c, o, "segmentsCreated", JS_NewUint32(c, r.segments_created));
  JS_SetPropertyStr(c, o, "eventsWritten", JS_NewInt64(c, static_cast<int64_t>(r.events_written)));
  return o;
}

static JSValue js_seg_export(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  const char* in = JS_ToCString(c, a[0]);
  const char* out = JS_ToCString(c, a[1]);
  uint8_t fmt = argc > 2 ? static_cast<uint8_t>(toUint32(c, a[2])) : 0;
  int64_t from = argc > 3 ? toInt64(c, a[3]) : 0;
  int64_t to = argc > 4 ? toInt64(c, a[4]) : 0;
  std::vector<uint32_t> syms;
  if (argc > 5 && JS_IsArray(c, a[5]))
  {
    JSValue lv = JS_GetPropertyStr(c, a[5], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lv);
    JS_FreeValue(c, lv);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[5], i);
      syms.push_back(toUint32(c, e));
      JS_FreeValue(c, e);
    }
  }
  FloxExportResult r = flox_segment_export(in, out, fmt, from, to,
                                           syms.empty() ? nullptr : syms.data(),
                                           static_cast<uint32_t>(syms.size()));
  JS_FreeCString(c, in);
  JS_FreeCString(c, out);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "success", JS_NewBool(c, r.success));
  JS_SetPropertyStr(c, o, "eventsExported", JS_NewInt64(c, static_cast<int64_t>(r.events_exported)));
  JS_SetPropertyStr(c, o, "bytesWritten", JS_NewInt64(c, static_cast<int64_t>(r.bytes_written)));
  return o;
}

static JSValue js_seg_validate_full(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  const char* path = JS_ToCString(c, a[0]);
  FloxSegmentValidation r = flox_segment_validate_full(path,
                                                       argc > 1 ? static_cast<uint8_t>(JS_ToBool(c, a[1])) : 1,
                                                       argc > 2 ? static_cast<uint8_t>(JS_ToBool(c, a[2])) : 1);
  JS_FreeCString(c, path);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "valid", JS_NewBool(c, r.valid));
  JS_SetPropertyStr(c, o, "headerValid", JS_NewBool(c, r.header_valid));
  JS_SetPropertyStr(c, o, "reportedEventCount", JS_NewInt64(c, static_cast<int64_t>(r.reported_event_count)));
  JS_SetPropertyStr(c, o, "actualEventCount", JS_NewInt64(c, static_cast<int64_t>(r.actual_event_count)));
  JS_SetPropertyStr(c, o, "hasIndex", JS_NewBool(c, r.has_index));
  JS_SetPropertyStr(c, o, "indexValid", JS_NewBool(c, r.index_valid));
  JS_SetPropertyStr(c, o, "tradesFound", JS_NewInt64(c, static_cast<int64_t>(r.trades_found)));
  JS_SetPropertyStr(c, o, "bookUpdatesFound", JS_NewInt64(c, static_cast<int64_t>(r.book_updates_found)));
  JS_SetPropertyStr(c, o, "crcErrors", JS_NewUint32(c, r.crc_errors));
  JS_SetPropertyStr(c, o, "timestampAnomalies", JS_NewUint32(c, r.timestamp_anomalies));
  return o;
}

static JSValue js_dataset_validate(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  const char* dir = JS_ToCString(c, a[0]);
  FloxDatasetValidation r = flox_dataset_validate(dir);
  JS_FreeCString(c, dir);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "valid", JS_NewBool(c, r.valid));
  JS_SetPropertyStr(c, o, "totalSegments", JS_NewUint32(c, r.total_segments));
  JS_SetPropertyStr(c, o, "validSegments", JS_NewUint32(c, r.valid_segments));
  JS_SetPropertyStr(c, o, "corruptedSegments", JS_NewUint32(c, r.corrupted_segments));
  JS_SetPropertyStr(c, o, "totalEvents", JS_NewInt64(c, static_cast<int64_t>(r.total_events)));
  JS_SetPropertyStr(c, o, "totalBytes", JS_NewInt64(c, static_cast<int64_t>(r.total_bytes)));
  JS_SetPropertyStr(c, o, "firstTimestamp", JS_NewInt64(c, r.first_timestamp));
  JS_SetPropertyStr(c, o, "lastTimestamp", JS_NewInt64(c, r.last_timestamp));
  return o;
}

static JSValue js_seg_recompress(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  const char* in = JS_ToCString(c, a[0]);
  const char* out = JS_ToCString(c, a[1]);
  uint8_t comp = argc > 2 ? static_cast<uint8_t>(toUint32(c, a[2])) : 1;
  JSValue ret = JS_NewBool(c, flox_segment_recompress(in, out, comp));
  JS_FreeCString(c, in);
  JS_FreeCString(c, out);
  return ret;
}

static JSValue js_seg_extract_symbols(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  const char* in = JS_ToCString(c, a[0]);
  const char* out = JS_ToCString(c, a[1]);
  std::vector<uint32_t> syms;
  if (JS_IsArray(c, a[2]))
  {
    JSValue lv = JS_GetPropertyStr(c, a[2], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lv);
    JS_FreeValue(c, lv);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[2], i);
      syms.push_back(toUint32(c, e));
      JS_FreeValue(c, e);
    }
  }
  uint64_t written = flox_segment_extract_symbols(in, out, syms.data(), static_cast<uint32_t>(syms.size()));
  JS_FreeCString(c, in);
  JS_FreeCString(c, out);
  return JS_NewInt64(c, static_cast<int64_t>(written));
}

static JSValue js_seg_extract_time(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  const char* in = JS_ToCString(c, a[0]);
  const char* out = JS_ToCString(c, a[1]);
  uint64_t written = flox_segment_extract_time_range(in, out, toInt64(c, a[2]), toInt64(c, a[3]));
  JS_FreeCString(c, in);
  JS_FreeCString(c, out);
  return JS_NewInt64(c, static_cast<int64_t>(written));
}

// ============================================================
// loadCsv -- read OHLCV CSV and return array of bar objects
// ============================================================

static int64_t detectTimestampNs(int64_t ts)
{
  // Thresholds match Python normalizeTimestamp and Codon _parse_ts.
  // Modern unix-ms timestamps (~1.78e12 in 2026) exceed 1e12, so the
  // seconds/ms boundary must be at 1e12 for seconds, 1e15 for ms.
  if (ts < 1'000'000'000'000LL)
  {
    return ts * 1'000'000'000LL;  // seconds → ns
  }
  if (ts < 1'000'000'000'000'000LL)
  {
    return ts * 1'000'000LL;  // ms → ns
  }
  if (ts < 1'000'000'000'000'000'000LL)
  {
    return ts * 1'000LL;  // us → ns
  }
  return ts;  // already ns
}

static JSValue js_load_csv(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  const char* path = JS_ToCString(c, a[0]);
  std::ifstream f(path);
  JS_FreeCString(c, path);
  if (!f.is_open())
  {
    return JS_ThrowTypeError(c, "loadCsv: file not found");
  }

  JSValue arr = JS_NewArray(c);
  uint32_t idx = 0;
  bool firstLine = true;
  std::string line;
  while (std::getline(f, line))
  {
    if (line.empty())
    {
      continue;
    }
    // Parse comma-separated: ts,open,high,low,close,volume
    std::vector<std::string> parts;
    std::istringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
      parts.push_back(tok);
    }
    if (parts.size() < 6)
    {
      continue;
    }
    // Detect header: first field non-numeric
    if (firstLine)
    {
      firstLine = false;
      char* ep;
      strtoll(parts[0].c_str(), &ep, 10);
      if (*ep != '\0')
      {
        continue;  // header row
      }
    }
    try
    {
      // Store ts in milliseconds — safe JS integer range (13 digits < 2^53).
      // Nanoseconds (19 digits) would lose precision as float64.
      int64_t ts_ns = detectTimestampNs(std::stoll(parts[0]));
      int64_t ts_ms = ts_ns / 1'000'000LL;
      double o = std::stod(parts[1]);
      double h = std::stod(parts[2]);
      double l = std::stod(parts[3]);
      double cl = std::stod(parts[4]);
      double v = std::stod(parts[5]);
      JSValue o2 = JS_NewObject(c);
      JS_SetPropertyStr(c, o2, "ts", JS_NewInt64(c, ts_ms));
      JS_SetPropertyStr(c, o2, "open", JS_NewFloat64(c, o));
      JS_SetPropertyStr(c, o2, "high", JS_NewFloat64(c, h));
      JS_SetPropertyStr(c, o2, "low", JS_NewFloat64(c, l));
      JS_SetPropertyStr(c, o2, "close", JS_NewFloat64(c, cl));
      JS_SetPropertyStr(c, o2, "volume", JS_NewFloat64(c, v));
      JS_SetPropertyUint32(c, arr, idx++, o2);
    }
    catch (...)
    {
      continue;
    }
  }
  return arr;
}

void registerFloxBindings(JSContext* ctx)
{
  // Register handle class
  JS_NewClassID(&jsHandleClassId);
  JS_NewClass(JS_GetRuntime(ctx), jsHandleClassId, &jsHandleClassDef);

  // console
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue console = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 1));
  JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunction(ctx, js_console_warn, "warn", 1));
  JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console_error, "error", 1));
  JS_SetPropertyStr(ctx, global, "console", console);
  JS_FreeValue(ctx, global);

  // Signal emission
  addGlobalFunc(ctx, "__flox_emit_market_buy", js_emit_market_buy, 3);
  addGlobalFunc(ctx, "__flox_emit_market_sell", js_emit_market_sell, 3);
  addGlobalFunc(ctx, "__flox_emit_limit_buy_tif", js_emit_limit_buy_tif, 5);
  addGlobalFunc(ctx, "__flox_emit_limit_sell_tif", js_emit_limit_sell_tif, 5);
  addGlobalFunc(ctx, "__flox_emit_cancel", js_emit_cancel, 2);
  addGlobalFunc(ctx, "__flox_emit_cancel_all", js_emit_cancel_all, 2);
  addGlobalFunc(ctx, "__flox_emit_modify", js_emit_modify, 4);
  addGlobalFunc(ctx, "__flox_emit_stop_market", js_emit_stop_market, 5);
  addGlobalFunc(ctx, "__flox_emit_stop_limit", js_emit_stop_limit, 6);
  addGlobalFunc(ctx, "__flox_emit_take_profit_market", js_emit_take_profit_market, 5);
  addGlobalFunc(ctx, "__flox_emit_take_profit_limit", js_emit_take_profit_limit, 6);
  addGlobalFunc(ctx, "__flox_emit_trailing_stop", js_emit_trailing_stop, 5);
  addGlobalFunc(ctx, "__flox_emit_trailing_stop_pct", js_emit_trailing_stop_pct, 5);
  addGlobalFunc(ctx, "__flox_emit_close_position", js_emit_close_position, 2);

  // Context queries
  addGlobalFunc(ctx, "__flox_position", js_position, 2);
  addGlobalFunc(ctx, "__flox_last_trade_price", js_last_trade_price, 2);
  addGlobalFunc(ctx, "__flox_best_bid", js_best_bid, 2);
  addGlobalFunc(ctx, "__flox_best_ask", js_best_ask, 2);
  addGlobalFunc(ctx, "__flox_mid_price", js_mid_price, 2);
  addGlobalFunc(ctx, "__flox_get_order_status", js_get_order_status, 2);

  // Batch indicators
  addGlobalFunc(ctx, "__flox_indicator_sma", js_indicator_sma, 2);
  addGlobalFunc(ctx, "__flox_indicator_ema", js_indicator_ema, 2);
  addGlobalFunc(ctx, "__flox_indicator_rsi", js_indicator_rsi, 2);
  addGlobalFunc(ctx, "__flox_indicator_atr", js_indicator_atr, 4);
  addGlobalFunc(ctx, "__flox_indicator_macd", js_indicator_macd, 4);
  addGlobalFunc(ctx, "__flox_indicator_bollinger", js_indicator_bollinger, 3);
  addGlobalFunc(ctx, "__flox_indicator_rma", js_indicator_rma, 2);
  addGlobalFunc(ctx, "__flox_indicator_dema", js_indicator_dema, 2);
  addGlobalFunc(ctx, "__flox_indicator_tema", js_indicator_tema, 2);
  addGlobalFunc(ctx, "__flox_indicator_kama", js_indicator_kama, 4);
  addGlobalFunc(ctx, "__flox_indicator_slope", js_indicator_slope, 2);
  addGlobalFunc(ctx, "__flox_indicator_adx", js_indicator_adx, 4);
  addGlobalFunc(ctx, "__flox_indicator_cci", js_indicator_cci, 4);
  addGlobalFunc(ctx, "__flox_indicator_stochastic", js_indicator_stochastic, 5);
  addGlobalFunc(ctx, "__flox_indicator_chop", js_indicator_chop, 4);
  addGlobalFunc(ctx, "__flox_indicator_obv", js_indicator_obv, 2);
  addGlobalFunc(ctx, "__flox_indicator_vwap", js_indicator_vwap, 3);
  addGlobalFunc(ctx, "__flox_indicator_cvd", js_indicator_cvd, 5);
  addGlobalFunc(ctx, "__flox_indicator_skewness", js_indicator_skewness, 2);
  addGlobalFunc(ctx, "__flox_indicator_kurtosis", js_indicator_kurtosis, 2);
  addGlobalFunc(ctx, "__flox_indicator_rolling_zscore", js_indicator_rolling_zscore, 2);
  addGlobalFunc(ctx, "__flox_indicator_shannon_entropy", js_indicator_shannon_entropy, 3);
  addGlobalFunc(ctx, "__flox_indicator_parkinson_vol", js_indicator_parkinson_vol, 3);
  addGlobalFunc(ctx, "__flox_indicator_rogers_satchell_vol", js_indicator_rogers_satchell_vol, 5);
  addGlobalFunc(ctx, "__flox_indicator_correlation", js_indicator_correlation, 3);

  // Order book
  addGlobalFunc(ctx, "__flox_book_create", js_book_create, 1);
  addGlobalFunc(ctx, "__flox_book_destroy", js_book_destroy, 1);
  addGlobalFunc(ctx, "__flox_book_apply_snapshot", js_book_apply_snapshot, 5);
  addGlobalFunc(ctx, "__flox_book_apply_delta", js_book_apply_delta, 5);
  addGlobalFunc(ctx, "__flox_book_best_bid", js_book_best_bid, 1);
  addGlobalFunc(ctx, "__flox_book_best_ask", js_book_best_ask, 1);
  addGlobalFunc(ctx, "__flox_book_mid", js_book_mid, 1);
  addGlobalFunc(ctx, "__flox_book_spread", js_book_spread, 1);
  addGlobalFunc(ctx, "__flox_book_clear", js_book_clear, 1);
  addGlobalFunc(ctx, "__flox_book_is_crossed", js_book_is_crossed, 1);

  // Executor
  addGlobalFunc(ctx, "__flox_executor_create", js_executor_create, 0);
  addGlobalFunc(ctx, "__flox_executor_destroy", js_executor_destroy, 1);
  addGlobalFunc(ctx, "__flox_executor_submit", js_executor_submit, 7);
  addGlobalFunc(ctx, "__flox_executor_on_bar", js_executor_on_bar, 3);
  addGlobalFunc(ctx, "__flox_executor_on_trade", js_executor_on_trade, 4);
  addGlobalFunc(ctx, "__flox_executor_advance_clock", js_executor_advance, 2);
  addGlobalFunc(ctx, "__flox_executor_fill_count", js_executor_fill_count, 1);
  addGlobalFunc(ctx, "__flox_executor_set_default_slippage",
                js_executor_set_default_slippage, 6);
  addGlobalFunc(ctx, "__flox_executor_set_symbol_slippage",
                js_executor_set_symbol_slippage, 7);
  addGlobalFunc(ctx, "__flox_executor_set_queue_model", js_executor_set_queue_model, 3);
  addGlobalFunc(ctx, "__flox_executor_on_trade_qty", js_executor_on_trade_qty, 5);
  addGlobalFunc(ctx, "__flox_executor_on_best_levels", js_executor_on_best_levels, 6);

  // Backtest result
  addGlobalFunc(ctx, "__flox_backtest_result_create", js_backtest_result_create, 6);
  addGlobalFunc(ctx, "__flox_backtest_result_destroy", js_backtest_result_destroy, 1);
  addGlobalFunc(ctx, "__flox_backtest_result_record_fill",
                js_backtest_result_record_fill, 7);
  addGlobalFunc(ctx, "__flox_backtest_result_ingest", js_backtest_result_ingest, 2);
  addGlobalFunc(ctx, "__flox_backtest_result_stats", js_backtest_result_stats, 1);
  addGlobalFunc(ctx, "__flox_backtest_result_equity_curve",
                js_backtest_result_equity_curve, 1);
  addGlobalFunc(ctx, "__flox_backtest_result_write_csv", js_backtest_result_write_csv, 2);

  // Position tracker
  addGlobalFunc(ctx, "__flox_pos_create", js_pos_create, 1);
  addGlobalFunc(ctx, "__flox_pos_destroy", js_pos_destroy, 1);
  addGlobalFunc(ctx, "__flox_pos_on_fill", js_pos_on_fill, 5);
  addGlobalFunc(ctx, "__flox_pos_position", js_pos_position, 2);
  addGlobalFunc(ctx, "__flox_pos_avg_entry", js_pos_avg_entry, 2);
  addGlobalFunc(ctx, "__flox_pos_pnl", js_pos_pnl, 2);
  addGlobalFunc(ctx, "__flox_pos_total_pnl", js_pos_total_pnl, 1);

  // Volume profile
  addGlobalFunc(ctx, "__flox_vprofile_create", js_vprofile_create, 1);
  addGlobalFunc(ctx, "__flox_vprofile_destroy", js_vprofile_destroy, 1);
  addGlobalFunc(ctx, "__flox_vprofile_add_trade", js_vprofile_add_trade, 4);
  addGlobalFunc(ctx, "__flox_vprofile_poc", js_vprofile_poc, 1);
  addGlobalFunc(ctx, "__flox_vprofile_vah", js_vprofile_vah, 1);
  addGlobalFunc(ctx, "__flox_vprofile_val", js_vprofile_val, 1);
  addGlobalFunc(ctx, "__flox_vprofile_total_volume", js_vprofile_total_vol, 1);
  addGlobalFunc(ctx, "__flox_vprofile_clear", js_vprofile_clear, 1);

  // Stats
  addGlobalFunc(ctx, "__flox_stat_correlation", js_stat_correlation, 2);

  // Stat extras
  // L3 book
  addGlobalFunc(ctx, "__flox_l3_create", [](JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue
                { return createHandleObject(c, flox_l3_book_create()); }, 0);
  addGlobalFunc(ctx, "__flox_l3_destroy", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  flox_l3_book_destroy(static_cast<FloxL3BookHandle>(getHandle(c, a[0])));
                  return JS_UNDEFINED; }, 1);
  addGlobalFunc(ctx, "__flox_l3_add_order", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return JS_NewInt32(
                      c, flox_l3_book_add_order(
                             static_cast<FloxL3BookHandle>(getHandle(c, a[0])),
                             static_cast<uint64_t>(toInt64(c, a[1])), toDouble(c, a[2]),
                             toDouble(c, a[3]), static_cast<uint8_t>(toUint32(c, a[4])))); }, 5);
  addGlobalFunc(ctx, "__flox_l3_remove_order", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return JS_NewInt32(
                      c, flox_l3_book_remove_order(
                             static_cast<FloxL3BookHandle>(getHandle(c, a[0])),
                             static_cast<uint64_t>(toInt64(c, a[1])))); }, 2);
  addGlobalFunc(ctx, "__flox_l3_modify_order", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return JS_NewInt32(
                      c, flox_l3_book_modify_order(
                             static_cast<FloxL3BookHandle>(getHandle(c, a[0])),
                             static_cast<uint64_t>(toInt64(c, a[1])), toDouble(c, a[2]))); }, 3);
  addGlobalFunc(ctx, "__flox_l3_best_bid", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  double p = 0;
                  if (flox_l3_book_best_bid(
                          static_cast<FloxL3BookHandle>(getHandle(c, a[0])), &p)){
                    return JS_NewFloat64(c, p);
}
                  return JS_NULL; }, 1);
  addGlobalFunc(ctx, "__flox_l3_best_ask", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  double p = 0;
                  if (flox_l3_book_best_ask(
                          static_cast<FloxL3BookHandle>(getHandle(c, a[0])), &p)){
                    return JS_NewFloat64(c, p);
}
                  return JS_NULL; }, 1);

  // Footprint (native, separate from volume profile)
  addGlobalFunc(ctx, "__flox_fp_create", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return createHandleObject(c, flox_footprint_create(toDouble(c, a[0]))); }, 1);
  addGlobalFunc(ctx, "__flox_fp_destroy", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  flox_footprint_destroy(static_cast<FloxFootprintHandle>(getHandle(c, a[0])));
                  return JS_UNDEFINED; }, 1);
  addGlobalFunc(ctx, "__flox_fp_add_trade", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  flox_footprint_add_trade(
                      static_cast<FloxFootprintHandle>(getHandle(c, a[0])), toDouble(c, a[1]),
                      toDouble(c, a[2]), static_cast<uint8_t>(toUint32(c, a[3])));
                  return JS_UNDEFINED; }, 4);
  addGlobalFunc(ctx, "__flox_fp_total_delta", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return JS_NewFloat64(
                      c, flox_footprint_total_delta(
                             static_cast<FloxFootprintHandle>(getHandle(c, a[0])))); }, 1);
  addGlobalFunc(ctx, "__flox_fp_total_volume", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return JS_NewFloat64(
                      c, flox_footprint_total_volume(
                             static_cast<FloxFootprintHandle>(getHandle(c, a[0])))); }, 1);
  addGlobalFunc(ctx, "__flox_fp_clear", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  flox_footprint_clear(static_cast<FloxFootprintHandle>(getHandle(c, a[0])));
                  return JS_UNDEFINED; }, 1);

  // Book level access
  addGlobalFunc(ctx, "__flox_book_get_bids", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  double prices[100], qtys[100];
                  uint32_t n = flox_book_get_bids(
                      static_cast<FloxBookHandle>(getHandle(c, a[0])), prices, qtys,
                      std::min(toUint32(c, a[1]), 100u));
                  JSValue arr = JS_NewArray(c);
                  for (uint32_t i = 0; i < n; i++)
                  {
                    JSValue level = JS_NewArray(c);
                    JS_SetPropertyUint32(c, level, 0, JS_NewFloat64(c, prices[i]));
                    JS_SetPropertyUint32(c, level, 1, JS_NewFloat64(c, qtys[i]));
                    JS_SetPropertyUint32(c, arr, i, level);
                  }
                  return arr; }, 2);
  addGlobalFunc(ctx, "__flox_book_get_asks", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  double prices[100], qtys[100];
                  uint32_t n = flox_book_get_asks(
                      static_cast<FloxBookHandle>(getHandle(c, a[0])), prices, qtys,
                      std::min(toUint32(c, a[1]), 100u));
                  JSValue arr = JS_NewArray(c);
                  for (uint32_t i = 0; i < n; i++)
                  {
                    JSValue level = JS_NewArray(c);
                    JS_SetPropertyUint32(c, level, 0, JS_NewFloat64(c, prices[i]));
                    JS_SetPropertyUint32(c, level, 1, JS_NewFloat64(c, qtys[i]));
                    JS_SetPropertyUint32(c, arr, i, level);
                  }
                  return arr; }, 2);

  // Bootstrap CI
  addGlobalFunc(ctx, "__flox_stat_bootstrap_ci", [](JSContext* c, JSValueConst, int argc, JSValueConst* a) -> JSValue
                {
                  auto data = jsArrayToDoubles(c, a[0]);
                  double conf = (argc > 1) ? toDouble(c, a[1]) : 0.95;
                  uint32_t samples = (argc > 2) ? toUint32(c, a[2]) : 10000;
                  double lo, med, hi;
                  flox_stat_bootstrap_ci(data.data(), data.size(), conf, samples, &lo, &med, &hi);
                  JSValue r = JS_NewObject(c);
                  JS_SetPropertyStr(c, r, "lower", JS_NewFloat64(c, lo));
                  JS_SetPropertyStr(c, r, "median", JS_NewFloat64(c, med));
                  JS_SetPropertyStr(c, r, "upper", JS_NewFloat64(c, hi));
                  return r; }, 3);

  // Permutation test
  addGlobalFunc(ctx, "__flox_stat_permutation_test", [](JSContext* c, JSValueConst, int argc, JSValueConst* a) -> JSValue
                {
                  auto g1 = jsArrayToDoubles(c, a[0]);
                  auto g2 = jsArrayToDoubles(c, a[1]);
                  uint32_t n = (argc > 2) ? toUint32(c, a[2]) : 10000;
                  return JS_NewFloat64(
                      c, flox_stat_permutation_test(g1.data(), g1.size(), g2.data(), g2.size(), n)); }, 3);

  // Segment ops
  addGlobalFunc(ctx, "__flox_segment_validate", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  const char* path = JS_ToCString(c, a[0]);
                  uint8_t r = flox_segment_validate(path);
                  JS_FreeCString(c, path);
                  return JS_NewBool(c, r); }, 1);
  addGlobalFunc(ctx, "__flox_segment_merge", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  const char* dir = JS_ToCString(c, a[0]);
                  const char* out = JS_ToCString(c, a[1]);
                  uint8_t r = flox_segment_merge(dir, out);
                  JS_FreeCString(c, dir);
                  JS_FreeCString(c, out);
                  return JS_NewBool(c, r); }, 2);

  addGlobalFunc(ctx, "__flox_stat_profit_factor", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  auto d = jsArrayToDoubles(c, a[0]);
                  return JS_NewFloat64(c, flox_stat_profit_factor(d.data(), d.size())); }, 1);
  addGlobalFunc(ctx, "__flox_stat_win_rate", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  auto d = jsArrayToDoubles(c, a[0]);
                  return JS_NewFloat64(c, flox_stat_win_rate(d.data(), d.size())); }, 1);

  // MarketProfile
  addGlobalFunc(ctx, "__flox_mp_create", js_mp_create, 3);
  addGlobalFunc(ctx, "__flox_mp_destroy", js_mp_destroy, 1);
  addGlobalFunc(ctx, "__flox_mp_add_trade", js_mp_add_trade, 5);
  addGlobalFunc(ctx, "__flox_mp_poc", js_mp_poc, 1);
  addGlobalFunc(ctx, "__flox_mp_vah", js_mp_vah, 1);
  addGlobalFunc(ctx, "__flox_mp_val", js_mp_val, 1);
  addGlobalFunc(ctx, "__flox_mp_ib_high", js_mp_ib_high, 1);
  addGlobalFunc(ctx, "__flox_mp_ib_low", js_mp_ib_low, 1);
  addGlobalFunc(ctx, "__flox_mp_is_poor_high", js_mp_is_poor_high, 1);
  addGlobalFunc(ctx, "__flox_mp_is_poor_low", js_mp_is_poor_low, 1);
  addGlobalFunc(ctx, "__flox_mp_clear", js_mp_clear, 1);

  // CompositeBook
  addGlobalFunc(ctx, "__flox_cb_create", js_cb_create, 0);
  addGlobalFunc(ctx, "__flox_cb_destroy", js_cb_destroy, 1);
  addGlobalFunc(ctx, "__flox_cb_best_bid", js_cb_best_bid, 2);
  addGlobalFunc(ctx, "__flox_cb_best_ask", js_cb_best_ask, 2);
  addGlobalFunc(ctx, "__flox_cb_has_arb", js_cb_has_arb, 2);
  addGlobalFunc(ctx, "__flox_cb_mark_stale", js_cb_mark_stale, 3);
  addGlobalFunc(ctx, "__flox_cb_check_staleness", js_cb_check_staleness, 3);

  // OrderTracker
  addGlobalFunc(ctx, "__flox_ot_create", js_ot_create, 0);
  addGlobalFunc(ctx, "__flox_ot_destroy", js_ot_destroy, 1);
  addGlobalFunc(ctx, "__flox_ot_submit", js_ot_submit, 6);
  addGlobalFunc(ctx, "__flox_ot_filled", js_ot_filled, 3);
  addGlobalFunc(ctx, "__flox_ot_canceled", js_ot_canceled, 2);
  addGlobalFunc(ctx, "__flox_ot_is_active", js_ot_is_active, 2);
  addGlobalFunc(ctx, "__flox_ot_active_count", js_ot_active_count, 1);
  addGlobalFunc(ctx, "__flox_ot_total_count", js_ot_total_count, 1);
  addGlobalFunc(ctx, "__flox_ot_prune", js_ot_prune, 1);

  // PositionGroupTracker
  addGlobalFunc(ctx, "__flox_pg_create", js_pg_create, 0);
  addGlobalFunc(ctx, "__flox_pg_destroy", js_pg_destroy, 1);
  addGlobalFunc(ctx, "__flox_pg_open", js_pg_open, 6);
  addGlobalFunc(ctx, "__flox_pg_close", js_pg_close, 3);
  addGlobalFunc(ctx, "__flox_pg_partial_close", js_pg_partial_close, 4);
  addGlobalFunc(ctx, "__flox_pg_net", js_pg_net, 2);
  addGlobalFunc(ctx, "__flox_pg_pnl", js_pg_pnl, 2);
  addGlobalFunc(ctx, "__flox_pg_total_pnl", js_pg_total_pnl, 1);
  addGlobalFunc(ctx, "__flox_pg_open_count", js_pg_open_count, 2);
  addGlobalFunc(ctx, "__flox_pg_prune", js_pg_prune, 1);

  // DataWriter
  addGlobalFunc(ctx, "__flox_dw_create", js_dw_create, 3);
  addGlobalFunc(ctx, "__flox_dw_destroy", js_dw_destroy, 1);
  addGlobalFunc(ctx, "__flox_dw_write_trade", js_dw_write_trade, 8);
  addGlobalFunc(ctx, "__flox_dw_flush", js_dw_flush, 1);
  addGlobalFunc(ctx, "__flox_dw_close", js_dw_close, 1);
  addGlobalFunc(ctx, "__flox_dw_stats", js_dw_stats, 1);

  // DataReader
  addGlobalFunc(ctx, "__flox_dr_create", js_dr_create, 1);
  addGlobalFunc(ctx, "__flox_dr_create_filtered", js_dr_create_filtered, 4);
  addGlobalFunc(ctx, "__flox_dr_destroy", js_dr_destroy, 1);
  addGlobalFunc(ctx, "__flox_dr_count", js_dr_count, 1);
  addGlobalFunc(ctx, "__flox_dr_summary", js_dr_summary, 1);
  addGlobalFunc(ctx, "__flox_dr_stats", js_dr_stats, 1);
  addGlobalFunc(ctx, "__flox_dr_read_trades", js_dr_read_trades, 2);

  // DataRecorder
  addGlobalFunc(ctx, "__flox_recorder_create", js_recorder_create, 3);
  addGlobalFunc(ctx, "__flox_recorder_destroy", js_recorder_destroy, 1);
  addGlobalFunc(ctx, "__flox_recorder_add_symbol", js_recorder_add_symbol, 7);
  addGlobalFunc(ctx, "__flox_recorder_start", js_recorder_start, 1);
  addGlobalFunc(ctx, "__flox_recorder_stop", js_recorder_stop, 1);
  addGlobalFunc(ctx, "__flox_recorder_flush", js_recorder_flush, 1);
  addGlobalFunc(ctx, "__flox_recorder_is_recording", js_recorder_is_recording, 1);

  // Partitioner
  addGlobalFunc(ctx, "__flox_part_create", js_part_create, 1);
  addGlobalFunc(ctx, "__flox_part_destroy", js_part_destroy, 1);
  addGlobalFunc(ctx, "__flox_part_by_time", js_part_by_time, 3);
  addGlobalFunc(ctx, "__flox_part_by_duration", js_part_by_duration, 3);
  addGlobalFunc(ctx, "__flox_part_by_calendar", js_part_by_calendar, 3);
  addGlobalFunc(ctx, "__flox_part_by_symbol", js_part_by_symbol, 2);
  addGlobalFunc(ctx, "__flox_part_per_symbol", js_part_per_symbol, 1);
  addGlobalFunc(ctx, "__flox_part_by_event_count", js_part_by_event_count, 2);

  // Aggregators
  addGlobalFunc(ctx, "__flox_agg_time", js_agg_time, 5);
  addGlobalFunc(ctx, "__flox_agg_tick", js_agg_tick, 5);
  addGlobalFunc(ctx, "__flox_agg_volume", js_agg_volume, 5);
  addGlobalFunc(ctx, "__flox_agg_range", js_agg_range, 5);
  addGlobalFunc(ctx, "__flox_agg_renko", js_agg_renko, 5);
  addGlobalFunc(ctx, "__flox_agg_heikin", js_agg_heikin, 5);

  // Extended segment ops
  addGlobalFunc(ctx, "__flox_seg_merge_dir", js_seg_merge_dir, 2);
  addGlobalFunc(ctx, "__flox_seg_split", js_seg_split, 5);
  addGlobalFunc(ctx, "__flox_seg_export", js_seg_export, 6);
  addGlobalFunc(ctx, "__flox_seg_validate_full", js_seg_validate_full, 3);
  addGlobalFunc(ctx, "__flox_dataset_validate", js_dataset_validate, 1);
  addGlobalFunc(ctx, "__flox_seg_recompress", js_seg_recompress, 3);
  addGlobalFunc(ctx, "__flox_seg_extract_symbols", js_seg_extract_symbols, 3);
  addGlobalFunc(ctx, "__flox_seg_extract_time", js_seg_extract_time, 4);

  // CSV loader
  addGlobalFunc(ctx, "__flox_load_csv", js_load_csv, 1);
}

}  // namespace flox
