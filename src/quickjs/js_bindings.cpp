#include "js_bindings.h"
#include "flox/capi/flox_capi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
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
// Multi-timeframe alignment helpers
// ============================================================

static JSValue jsBarFromFlox(JSContext* ctx, const FloxBar& b)
{
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "open", JS_NewFloat64(ctx, flox_price_to_double(b.open_raw)));
  JS_SetPropertyStr(ctx, obj, "high", JS_NewFloat64(ctx, flox_price_to_double(b.high_raw)));
  JS_SetPropertyStr(ctx, obj, "low", JS_NewFloat64(ctx, flox_price_to_double(b.low_raw)));
  JS_SetPropertyStr(ctx, obj, "close", JS_NewFloat64(ctx, flox_price_to_double(b.close_raw)));
  JS_SetPropertyStr(ctx, obj, "volume",
                    JS_NewFloat64(ctx, flox_quantity_to_double(b.volume_raw)));
  JS_SetPropertyStr(ctx, obj, "startNs",
                    JS_NewFloat64(ctx, static_cast<double>(b.start_time_ns)));
  JS_SetPropertyStr(ctx, obj, "endNs",
                    JS_NewFloat64(ctx, static_cast<double>(b.end_time_ns)));
  return obj;
}

static JSValue js_strategy_last_closed_bar(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t bt = toUint32(ctx, argv[2]);
  uint64_t param = static_cast<uint64_t>(toInt64(ctx, argv[3]));
  FloxBar bar{};
  uint8_t ok = flox_strategy_last_closed_bar(h, sym, static_cast<uint8_t>(bt), param, &bar);
  if (!ok)
  {
    return JS_NULL;
  }
  return jsBarFromFlox(ctx, bar);
}

static JSValue js_strategy_last_n_closed_bars(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t bt = toUint32(ctx, argv[2]);
  uint64_t param = static_cast<uint64_t>(toInt64(ctx, argv[3]));
  uint32_t n = toUint32(ctx, argv[4]);
  std::vector<FloxBar> bars(n);
  uint32_t got = flox_strategy_last_n_closed_bars(h, sym, static_cast<uint8_t>(bt), param,
                                                  bars.data(), n);
  JSValue arr = JS_NewArray(ctx);
  for (uint32_t i = 0; i < got; ++i)
  {
    JS_SetPropertyUint32(ctx, arr, i, jsBarFromFlox(ctx, bars[i]));
  }
  return arr;
}

static JSValue js_strategy_get_bar_ring_capacity(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  return JS_NewUint32(ctx, flox_strategy_get_bar_ring_capacity(h));
}

static JSValue js_strategy_set_bar_ring_capacity(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t cap = toUint32(ctx, argv[1]);
  flox_strategy_set_bar_ring_capacity(h, cap);
  return JS_UNDEFINED;
}

// ============================================================
// Multi-feed clock (W6-T021)
// ============================================================

static JSValue js_feed_clock_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 6 || !JS_IsArray(ctx, argv[0]))
  {
    return JS_ThrowTypeError(ctx, "feed_clock_create(symbols[], count, policy, timeoutMs, leader, budgetMs)");
  }
  uint32_t count = toUint32(ctx, argv[1]);
  std::vector<uint32_t> sv(count);
  for (uint32_t i = 0; i < count; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
    sv[i] = toUint32(ctx, v);
    JS_FreeValue(ctx, v);
  }
  uint32_t policy = toUint32(ctx, argv[2]);
  int64_t timeout = toInt64(ctx, argv[3]);
  uint32_t leader = toUint32(ctx, argv[4]);
  int64_t budget = toInt64(ctx, argv[5]);
  void* h = flox_feed_clock_create(sv.data(), count, static_cast<uint8_t>(policy), timeout,
                                   leader, budget);
  return JS_NewBigInt64(ctx, reinterpret_cast<int64_t>(h));
}

static JSValue js_feed_clock_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  int64_t hv = 0;
  JS_ToBigInt64(ctx, &hv, argv[0]);
  flox_feed_clock_destroy(reinterpret_cast<void*>(hv));
  return JS_UNDEFINED;
}

static JSValue js_feed_clock_symbol_count(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  int64_t hv = 0;
  JS_ToBigInt64(ctx, &hv, argv[0]);
  return JS_NewUint32(ctx, flox_feed_clock_symbol_count(reinterpret_cast<void*>(hv)));
}

static JSValue js_feed_clock_symbol_at(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  int64_t hv = 0;
  JS_ToBigInt64(ctx, &hv, argv[0]);
  uint32_t idx = toUint32(ctx, argv[1]);
  return JS_NewUint32(ctx, flox_feed_clock_symbol_at(reinterpret_cast<void*>(hv), idx));
}

static JSValue js_feed_clock_tick(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  int64_t hv = 0;
  JS_ToBigInt64(ctx, &hv, argv[0]);
  int64_t ts = toInt64(ctx, argv[1]);
  uint32_t sym = toUint32(ctx, argv[2]);
  return JS_NewUint32(ctx, flox_feed_clock_tick(reinterpret_cast<void*>(hv), ts, sym));
}

static JSValue js_feed_clock_last_fired(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  int64_t hv = 0;
  JS_ToBigInt64(ctx, &hv, argv[0]);
  return JS_NewUint32(ctx, flox_feed_clock_last_fired(reinterpret_cast<void*>(hv)));
}

static JSValue js_feed_clock_last_triggered_by(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  int64_t hv = 0;
  JS_ToBigInt64(ctx, &hv, argv[0]);
  return JS_NewUint32(ctx, flox_feed_clock_last_triggered_by(reinterpret_cast<void*>(hv)));
}

static JSValue js_feed_clock_last_seen_at(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  int64_t hv = 0;
  JS_ToBigInt64(ctx, &hv, argv[0]);
  uint32_t idx = toUint32(ctx, argv[1]);
  return JS_NewBigInt64(
      ctx, flox_feed_clock_last_seen_at(reinterpret_cast<void*>(hv), idx));
}

static JSValue js_feed_clock_staleness_at(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  int64_t hv = 0;
  JS_ToBigInt64(ctx, &hv, argv[0]);
  uint32_t idx = toUint32(ctx, argv[1]);
  return JS_NewBigInt64(
      ctx, flox_feed_clock_staleness_at(reinterpret_cast<void*>(hv), idx));
}

static JSValue js_feed_clock_reset(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  int64_t hv = 0;
  JS_ToBigInt64(ctx, &hv, argv[0]);
  flox_feed_clock_reset(reinterpret_cast<void*>(hv));
  return JS_UNDEFINED;
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

static JSValue js_indicator_adf(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto input = jsArrayToDoubles(ctx, argv[0]);
  uint32_t maxLag = argc > 1 ? toUint32(ctx, argv[1]) : 4;
  const char* reg = "c";
  if (argc > 2 && JS_IsString(argv[2]))
  {
    reg = JS_ToCString(ctx, argv[2]);
  }
  double testStat = 0.0;
  double pValue = 0.0;
  size_t usedLag = 0;
  flox_indicator_adf(input.data(), input.size(), maxLag, reg, &testStat, &pValue, &usedLag);
  if (argc > 2 && JS_IsString(argv[2]))
  {
    JS_FreeCString(ctx, reg);
  }
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "test_stat", JS_NewFloat64(ctx, testStat));
  JS_SetPropertyStr(ctx, obj, "p_value", JS_NewFloat64(ctx, pValue));
  JS_SetPropertyStr(ctx, obj, "used_lag", JS_NewUint32(ctx, static_cast<uint32_t>(usedLag)));
  return obj;
}

static JSValue js_indicator_autocorrelation(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  auto in = jsArrayToDoubles(ctx, argv[0]);
  uint32_t window = toUint32(ctx, argv[1]);
  uint32_t lag = toUint32(ctx, argv[2]);
  size_t len = in.size();
  std::vector<double> output(len);
  flox_indicator_autocorrelation(in.data(), len, window, lag, output.data());
  return jsArrayFromDoubles(ctx, output);
}

// ============================================================
// Targets (forward-looking labels, batch only)
// ============================================================

static JSValue js_target_future_return(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto close = jsArrayToDoubles(ctx, argv[0]);
  uint32_t horizon = toUint32(ctx, argv[1]);
  size_t len = close.size();
  std::vector<double> output(len);
  flox_target_future_return(close.data(), len, horizon, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_target_future_ctc_volatility(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  auto close = jsArrayToDoubles(ctx, argv[0]);
  uint32_t horizon = toUint32(ctx, argv[1]);
  size_t len = close.size();
  std::vector<double> output(len);
  flox_target_future_ctc_volatility(close.data(), len, horizon, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_target_future_linear_slope(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  auto close = jsArrayToDoubles(ctx, argv[0]);
  uint32_t horizon = toUint32(ctx, argv[1]);
  size_t len = close.size();
  std::vector<double> output(len);
  flox_target_future_linear_slope(close.data(), len, horizon, output.data());
  return jsArrayFromDoubles(ctx, output);
}

// ============================================================
// IndicatorGraph (batch) bindings
// ============================================================

namespace
{

struct JsGraphNodeState
{
  JSContext* ctx;
  JSValue fn;
  JSValue thisObj;
  std::vector<double> buffer;
};

struct JsGraphState
{
  FloxIndicatorGraphHandle handle;
  std::vector<std::unique_ptr<JsGraphNodeState>> nodes;
};

static const double* js_graph_node_trampoline(void* user_data, FloxIndicatorGraphHandle,
                                              uint32_t symbol, size_t* out_len)
{
  auto* st = static_cast<JsGraphNodeState*>(user_data);
  JSValue args[2] = {JS_DupValue(st->ctx, st->thisObj), JS_NewUint32(st->ctx, symbol)};
  JSValue result = JS_Call(st->ctx, st->fn, JS_UNDEFINED, 2, args);
  JS_FreeValue(st->ctx, args[0]);
  JS_FreeValue(st->ctx, args[1]);
  if (JS_IsException(result))
  {
    JS_FreeValue(st->ctx, result);
    *out_len = 0;
    return nullptr;
  }
  st->buffer = jsArrayToDoubles(st->ctx, result);
  JS_FreeValue(st->ctx, result);
  *out_len = st->buffer.size();
  return st->buffer.data();
}

}  // namespace

static JSValue js_graph_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  auto* st = new JsGraphState{flox_indicator_graph_create(), {}};
  return createHandleObject(ctx, st);
}

static JSValue js_graph_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsGraphState*>(getHandle(ctx, argv[0]));
  if (!st)
  {
    return JS_UNDEFINED;
  }
  for (auto& node : st->nodes)
  {
    JS_FreeValue(node->ctx, node->fn);
    JS_FreeValue(node->ctx, node->thisObj);
  }
  flox_indicator_graph_destroy(st->handle);
  delete st;
  return JS_UNDEFINED;
}

static JSValue js_graph_set_bars(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto* st = static_cast<JsGraphState*>(getHandle(ctx, argv[0]));
  uint32_t sym = toUint32(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  std::vector<double> high, low, volume;
  const double* hp = nullptr;
  const double* lp = nullptr;
  const double* vp = nullptr;
  if (argc > 3 && !JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3]))
  {
    high = jsArrayToDoubles(ctx, argv[3]);
    hp = high.data();
  }
  if (argc > 4 && !JS_IsNull(argv[4]) && !JS_IsUndefined(argv[4]))
  {
    low = jsArrayToDoubles(ctx, argv[4]);
    lp = low.data();
  }
  if (argc > 5 && !JS_IsNull(argv[5]) && !JS_IsUndefined(argv[5]))
  {
    volume = jsArrayToDoubles(ctx, argv[5]);
    vp = volume.data();
  }
  flox_indicator_graph_set_bars(st->handle, sym, close.data(), hp, lp, vp, close.size());
  return JS_UNDEFINED;
}

static JSValue js_graph_add_node(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsGraphState*>(getHandle(ctx, argv[0]));
  const char* name = JS_ToCString(ctx, argv[1]);
  std::string nameStr(name);
  JS_FreeCString(ctx, name);

  std::vector<std::string> deps;
  if (JS_IsArray(ctx, argv[2]))
  {
    uint32_t depsLen = 0;
    {
      JSValue lenVal = JS_GetPropertyStr(ctx, argv[2], "length");
      JS_ToUint32(ctx, &depsLen, lenVal);
      JS_FreeValue(ctx, lenVal);
    }
    for (uint32_t i = 0; i < depsLen; ++i)
    {
      JSValue v = JS_GetPropertyUint32(ctx, argv[2], i);
      const char* s = JS_ToCString(ctx, v);
      deps.emplace_back(s);
      JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, v);
    }
  }

  std::vector<const char*> depPtrs;
  depPtrs.reserve(deps.size());
  for (const auto& d : deps)
  {
    depPtrs.push_back(d.c_str());
  }

  // Capture the JS callable + the thisObj (graph wrapper).
  auto node = std::make_unique<JsGraphNodeState>();
  node->ctx = ctx;
  node->fn = JS_DupValue(ctx, argv[3]);
  node->thisObj = JS_DupValue(ctx, argv[4]);

  flox_indicator_graph_add_node(st->handle, nameStr.c_str(),
                                deps.empty() ? nullptr : depPtrs.data(), deps.size(),
                                js_graph_node_trampoline, node.get());
  st->nodes.push_back(std::move(node));
  return JS_UNDEFINED;
}

static JSValue js_graph_require(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsGraphState*>(getHandle(ctx, argv[0]));
  uint32_t sym = toUint32(ctx, argv[1]);
  const char* name = JS_ToCString(ctx, argv[2]);
  size_t len = 0;
  const double* p = flox_indicator_graph_require(st->handle, sym, name, &len);
  JS_FreeCString(ctx, name);
  if (!p)
  {
    return JS_ThrowTypeError(ctx, "graph: require failed");
  }
  return jsArrayFromDoubles(ctx, std::vector<double>(p, p + len));
}

static JSValue js_graph_get(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsGraphState*>(getHandle(ctx, argv[0]));
  uint32_t sym = toUint32(ctx, argv[1]);
  const char* name = JS_ToCString(ctx, argv[2]);
  size_t len = 0;
  const double* p = flox_indicator_graph_get(st->handle, sym, name, &len);
  JS_FreeCString(ctx, name);
  if (!p)
  {
    return JS_NULL;
  }
  return jsArrayFromDoubles(ctx, std::vector<double>(p, p + len));
}

#define GRAPH_FIELD(field)                                                               \
  static JSValue js_graph_##field(JSContext* ctx, JSValueConst, int, JSValueConst* argv) \
  {                                                                                      \
    auto* st = static_cast<JsGraphState*>(getHandle(ctx, argv[0]));                      \
    uint32_t sym = toUint32(ctx, argv[1]);                                               \
    size_t len = 0;                                                                      \
    const double* p = flox_indicator_graph_##field(st->handle, sym, &len);               \
    return jsArrayFromDoubles(ctx, std::vector<double>(p, p + len));                     \
  }
GRAPH_FIELD(close)
GRAPH_FIELD(high)
GRAPH_FIELD(low)
GRAPH_FIELD(volume)
#undef GRAPH_FIELD

static JSValue js_graph_invalidate(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsGraphState*>(getHandle(ctx, argv[0]));
  flox_indicator_graph_invalidate(st->handle, toUint32(ctx, argv[1]));
  return JS_UNDEFINED;
}

static JSValue js_graph_invalidate_all(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsGraphState*>(getHandle(ctx, argv[0]));
  flox_indicator_graph_invalidate_all(st->handle);
  return JS_UNDEFINED;
}

// ============================================================
// StreamingIndicatorGraph bindings
// ============================================================

namespace
{

struct JsStreamingState
{
  FloxStreamingGraphHandle handle;
  std::vector<std::unique_ptr<JsGraphNodeState>> nodes;
};

static const double* js_streaming_node_trampoline(void* user_data, FloxIndicatorGraphHandle,
                                                  uint32_t symbol, size_t* out_len)
{
  auto* st = static_cast<JsGraphNodeState*>(user_data);
  JSValue args[2] = {JS_DupValue(st->ctx, st->thisObj), JS_NewUint32(st->ctx, symbol)};
  JSValue result = JS_Call(st->ctx, st->fn, JS_UNDEFINED, 2, args);
  JS_FreeValue(st->ctx, args[0]);
  JS_FreeValue(st->ctx, args[1]);
  if (JS_IsException(result))
  {
    JS_FreeValue(st->ctx, result);
    *out_len = 0;
    return nullptr;
  }
  st->buffer = jsArrayToDoubles(st->ctx, result);
  JS_FreeValue(st->ctx, result);
  *out_len = st->buffer.size();
  return st->buffer.data();
}

}  // namespace

static JSValue js_streaming_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  auto* st = new JsStreamingState{flox_streaming_graph_create(), {}};
  return createHandleObject(ctx, st);
}

static JSValue js_streaming_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsStreamingState*>(getHandle(ctx, argv[0]));
  if (!st)
  {
    return JS_UNDEFINED;
  }
  for (auto& node : st->nodes)
  {
    JS_FreeValue(node->ctx, node->fn);
    JS_FreeValue(node->ctx, node->thisObj);
  }
  flox_streaming_graph_destroy(st->handle);
  delete st;
  return JS_UNDEFINED;
}

static JSValue js_streaming_add_node(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsStreamingState*>(getHandle(ctx, argv[0]));
  const char* name = JS_ToCString(ctx, argv[1]);
  std::string nameStr(name);
  JS_FreeCString(ctx, name);

  std::vector<std::string> deps;
  if (JS_IsArray(ctx, argv[2]))
  {
    uint32_t depsLen = 0;
    {
      JSValue lenVal = JS_GetPropertyStr(ctx, argv[2], "length");
      JS_ToUint32(ctx, &depsLen, lenVal);
      JS_FreeValue(ctx, lenVal);
    }
    for (uint32_t i = 0; i < depsLen; ++i)
    {
      JSValue v = JS_GetPropertyUint32(ctx, argv[2], i);
      const char* s = JS_ToCString(ctx, v);
      deps.emplace_back(s);
      JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, v);
    }
  }

  std::vector<const char*> depPtrs;
  depPtrs.reserve(deps.size());
  for (const auto& d : deps)
  {
    depPtrs.push_back(d.c_str());
  }

  auto node = std::make_unique<JsGraphNodeState>();
  node->ctx = ctx;
  node->fn = JS_DupValue(ctx, argv[3]);
  node->thisObj = JS_DupValue(ctx, argv[4]);

  flox_streaming_graph_add_node(st->handle, nameStr.c_str(),
                                deps.empty() ? nullptr : depPtrs.data(), deps.size(),
                                js_streaming_node_trampoline, node.get());
  st->nodes.push_back(std::move(node));
  return JS_UNDEFINED;
}

static JSValue js_streaming_step(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto* st = static_cast<JsStreamingState*>(getHandle(ctx, argv[0]));
  uint32_t sym = toUint32(ctx, argv[1]);
  double c = toDouble(ctx, argv[2]);
  double h = argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])
                 ? toDouble(ctx, argv[3])
                 : c;
  double l = argc > 4 && !JS_IsUndefined(argv[4]) && !JS_IsNull(argv[4])
                 ? toDouble(ctx, argv[4])
                 : c;
  double v = argc > 5 && !JS_IsUndefined(argv[5]) && !JS_IsNull(argv[5])
                 ? toDouble(ctx, argv[5])
                 : 0.0;
  flox_streaming_graph_step(st->handle, sym, c, h, l, c, v);
  return JS_UNDEFINED;
}

static JSValue js_streaming_current(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsStreamingState*>(getHandle(ctx, argv[0]));
  uint32_t sym = toUint32(ctx, argv[1]);
  const char* name = JS_ToCString(ctx, argv[2]);
  double val = flox_streaming_graph_current(st->handle, sym, name);
  JS_FreeCString(ctx, name);
  return JS_NewFloat64(ctx, val);
}

static JSValue js_streaming_bar_count(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsStreamingState*>(getHandle(ctx, argv[0]));
  uint32_t sym = toUint32(ctx, argv[1]);
  return JS_NewUint32(ctx, flox_streaming_graph_bar_count(st->handle, sym));
}

static JSValue js_streaming_reset(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsStreamingState*>(getHandle(ctx, argv[0]));
  flox_streaming_graph_reset(st->handle, toUint32(ctx, argv[1]));
  return JS_UNDEFINED;
}

static JSValue js_streaming_reset_all(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = static_cast<JsStreamingState*>(getHandle(ctx, argv[0]));
  flox_streaming_graph_reset_all(st->handle);
  return JS_UNDEFINED;
}

#define STREAMING_FIELD(field)                                                               \
  static JSValue js_streaming_##field(JSContext* ctx, JSValueConst, int, JSValueConst* argv) \
  {                                                                                          \
    auto* st = static_cast<JsStreamingState*>(getHandle(ctx, argv[0]));                      \
    uint32_t sym = toUint32(ctx, argv[1]);                                                   \
    size_t len = 0;                                                                          \
    const double* p = flox_streaming_graph_##field(st->handle, sym, &len);                   \
    return jsArrayFromDoubles(ctx, std::vector<double>(p, p + len));                         \
  }
STREAMING_FIELD(close)
STREAMING_FIELD(high)
STREAMING_FIELD(low)
STREAMING_FIELD(volume)
#undef STREAMING_FIELD

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
  return createHandleObject(ctx, flox_simulated_executor_create());
}
static JSValue js_executor_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_simulated_executor_destroy(static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_executor_submit(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0]));
  flox_simulated_executor_submit_order(h, static_cast<uint64_t>(toInt64(ctx, argv[1])),
                                       static_cast<uint8_t>(toUint32(ctx, argv[2])),
                                       toDouble(ctx, argv[3]), toDouble(ctx, argv[4]),
                                       static_cast<uint8_t>(toUint32(ctx, argv[5])),
                                       toUint32(ctx, argv[6]));
  return JS_UNDEFINED;
}
static JSValue js_executor_submit_ex(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0]));
  flox_simulated_executor_submit_order_ex(
      h, static_cast<uint64_t>(toInt64(ctx, argv[1])),
      static_cast<uint8_t>(toUint32(ctx, argv[2])), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), static_cast<uint8_t>(toUint32(ctx, argv[5])),
      toUint32(ctx, argv[6]), static_cast<uint8_t>(toUint32(ctx, argv[7])),
      static_cast<uint8_t>(toUint32(ctx, argv[8])), toInt64(ctx, argv[9]));
  return JS_UNDEFINED;
}
static JSValue js_executor_submit_bracket(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  flox_simulated_executor_submit_bracket(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])),
      toUint32(ctx, argv[2]),
      static_cast<uint8_t>(toUint32(ctx, argv[3])),
      static_cast<uint8_t>(toUint32(ctx, argv[4])),
      toDouble(ctx, argv[5]), toDouble(ctx, argv[6]),
      static_cast<uint8_t>(toUint32(ctx, argv[7])),
      static_cast<uint8_t>(toUint32(ctx, argv[8])),
      toDouble(ctx, argv[9]),
      static_cast<uint8_t>(toUint32(ctx, argv[10])),
      static_cast<uint8_t>(toUint32(ctx, argv[11])),
      toDouble(ctx, argv[12]));
  return JS_UNDEFINED;
}
static JSValue js_executor_cancel_bracket(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  flox_simulated_executor_cancel_bracket(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_bracket_state(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  return JS_NewUint32(ctx, flox_simulated_executor_bracket_state(
                               static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                               static_cast<uint64_t>(toInt64(ctx, argv[1]))));
}
static JSValue js_executor_set_bracket_child_arm_mode(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  flox_simulated_executor_set_bracket_child_arm_mode(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint8_t>(toUint32(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_bar(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_simulated_executor_on_bar(static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                 toUint32(ctx, argv[1]), toDouble(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_trade(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_simulated_executor_on_trade(static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                   toUint32(ctx, argv[1]), toDouble(ctx, argv[2]),
                                   static_cast<uint8_t>(toUint32(ctx, argv[3])));
  return JS_UNDEFINED;
}
static JSValue js_executor_advance(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_simulated_executor_advance_clock(static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                        toInt64(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_executor_fill_count(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewUint32(
      ctx, flox_simulated_executor_fill_count(static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0]))));
}

static JSValue js_executor_set_default_slippage(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  flox_simulated_executor_set_default_slippage(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<int32_t>(toInt64(ctx, argv[1])),
      static_cast<int32_t>(toInt64(ctx, argv[2])), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), toDouble(ctx, argv[5]));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_symbol_slippage(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  flox_simulated_executor_set_symbol_slippage(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), toUint32(ctx, argv[1]),
      static_cast<int32_t>(toInt64(ctx, argv[2])),
      static_cast<int32_t>(toInt64(ctx, argv[3])), toDouble(ctx, argv[4]),
      toDouble(ctx, argv[5]), toDouble(ctx, argv[6]));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_queue_model(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  flox_simulated_executor_set_queue_model(static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                          static_cast<int32_t>(toInt64(ctx, argv[1])),
                                          toUint32(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_queue_fifo_top_n(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  flox_simulated_executor_set_queue_fifo_top_n(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toUint32(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_submit_ack(JSContext* ctx, JSValueConst, int argc,
                                          JSValueConst* argv)
{
  flox_simulated_executor_set_submit_ack_latency(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), argc > 2 ? toInt64(ctx, argv[2]) : 0);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_cancel_ack(JSContext* ctx, JSValueConst, int argc,
                                          JSValueConst* argv)
{
  flox_simulated_executor_set_cancel_ack_latency(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), argc > 2 ? toInt64(ctx, argv[2]) : 0);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_replace_ack(JSContext* ctx, JSValueConst, int argc,
                                           JSValueConst* argv)
{
  flox_simulated_executor_set_replace_ack_latency(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), argc > 2 ? toInt64(ctx, argv[2]) : 0);
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, flox_latency_distribution_create());
}
static JSValue js_latency_dist_destroy(JSContext* ctx, JSValueConst, int,
                                       JSValueConst* argv)
{
  flox_latency_distribution_destroy(
      static_cast<FloxLatencyDistributionHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_set_constant(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  flox_latency_distribution_set_constant(
      static_cast<FloxLatencyDistributionHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_set_uniform(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  flox_latency_distribution_set_uniform(
      static_cast<FloxLatencyDistributionHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), toInt64(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_set_lognormal(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  double sigma = 0.0;
  JS_ToFloat64(ctx, &sigma, argv[2]);
  flox_latency_distribution_set_lognormal(
      static_cast<FloxLatencyDistributionHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), sigma);
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_set_empirical(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  auto h = static_cast<FloxLatencyDistributionHandle>(getHandle(ctx, argv[0]));
  uint32_t n = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
  JS_ToUint32(ctx, &n, lenVal);
  JS_FreeValue(ctx, lenVal);
  std::vector<int64_t> samples;
  samples.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
    samples.push_back(toInt64(ctx, v));
    JS_FreeValue(ctx, v);
  }
  flox_latency_distribution_set_empirical(
      h, samples.empty() ? nullptr : samples.data(), n);
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_set_burst(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  double rho = 0.0;
  JS_ToFloat64(ctx, &rho, argv[1]);
  flox_latency_distribution_set_burst_correlation(
      static_cast<FloxLatencyDistributionHandle>(getHandle(ctx, argv[0])), rho);
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_median_ns(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  return JS_NewInt64(ctx, flox_latency_distribution_median_ns(
                              static_cast<FloxLatencyDistributionHandle>(
                                  getHandle(ctx, argv[0]))));
}
static JSValue js_executor_set_submit_ack_dist(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  flox_simulated_executor_set_submit_ack_latency_distribution(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<FloxLatencyDistributionHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_cancel_ack_dist(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  flox_simulated_executor_set_cancel_ack_latency_distribution(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<FloxLatencyDistributionHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_replace_ack_dist(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  flox_simulated_executor_set_replace_ack_latency_distribution(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<FloxLatencyDistributionHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}

static JSValue js_executor_apply_latency_profile(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  const char* name = JS_ToCString(ctx, argv[1]);
  flox_simulated_executor_apply_latency_profile(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), name);
  if (name)
  {
    JS_FreeCString(ctx, name);
  }
  return JS_UNDEFINED;
}
static JSValue js_executor_set_stp_mode(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  uint8_t mode = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  flox_simulated_executor_set_stp_mode(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), mode);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_fok_mode(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  uint8_t code = 0;
  if (JS_IsString(argv[1]))
  {
    const char* s = JS_ToCString(ctx, argv[1]);
    if (s != nullptr)
    {
      std::string name(s);
      JS_FreeCString(ctx, s);
      if (name == "single_price")
      {
        code = 1;
      }
    }
  }
  else
  {
    code = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  }
  flox_simulated_executor_set_fok_mode(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), code);
  return JS_UNDEFINED;
}
static JSValue js_executor_fok_mode(JSContext* ctx, JSValueConst, int,
                                    JSValueConst* argv)
{
  const uint8_t code = flox_simulated_executor_fok_mode(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])));
  return JS_NewUint32(ctx, code);
}
static JSValue js_rate_limit_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, flox_rate_limit_policy_create());
}
static JSValue js_rate_limit_destroy(JSContext* ctx, JSValueConst, int,
                                     JSValueConst* argv)
{
  flox_rate_limit_policy_destroy(
      static_cast<FloxRateLimitPolicyHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_rate_limit_add_bucket(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  auto h = static_cast<FloxRateLimitPolicyHandle>(getHandle(ctx, argv[0]));
  const char* name = JS_ToCString(ctx, argv[1]);
  int64_t window = toInt64(ctx, argv[2]);
  uint32_t cap = toUint32(ctx, argv[3]);
  uint32_t sw = toUint32(ctx, argv[4]);
  uint32_t cw = toUint32(ctx, argv[5]);
  uint32_t rw = toUint32(ctx, argv[6]);
  flox_rate_limit_policy_add_bucket(h, name ? name : "bucket", window, cap, sw, cw, rw);
  if (name)
  {
    JS_FreeCString(ctx, name);
  }
  return JS_UNDEFINED;
}
static JSValue js_rate_limit_add_bucket_family(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  // (handle, name, window_ns, capacity, submit_w, cancel_w, replace_w, family, query_w)
  auto h = static_cast<FloxRateLimitPolicyHandle>(getHandle(ctx, argv[0]));
  const char* name = JS_ToCString(ctx, argv[1]);
  int64_t window = toInt64(ctx, argv[2]);
  uint32_t cap = toUint32(ctx, argv[3]);
  uint32_t sw = toUint32(ctx, argv[4]);
  uint32_t cw = toUint32(ctx, argv[5]);
  uint32_t rw = toUint32(ctx, argv[6]);
  uint8_t family = static_cast<uint8_t>(toUint32(ctx, argv[7]));
  uint32_t qw = toUint32(ctx, argv[8]);
  flox_rate_limit_policy_add_bucket_family(h, name ? name : "bucket", window, cap,
                                           sw, cw, rw, family, qw);
  if (name)
  {
    JS_FreeCString(ctx, name);
  }
  return JS_UNDEFINED;
}
static JSValue js_rate_limit_set_ban(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_rate_limit_policy_set_ban(
      static_cast<FloxRateLimitPolicyHandle>(getHandle(ctx, argv[0])),
      toUint32(ctx, argv[1]), toInt64(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_rate_limit_load_profile(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  const char* name = JS_ToCString(ctx, argv[1]);
  flox_rate_limit_policy_load_profile(
      static_cast<FloxRateLimitPolicyHandle>(getHandle(ctx, argv[0])), name);
  if (name)
  {
    JS_FreeCString(ctx, name);
  }
  return JS_UNDEFINED;
}
static JSValue js_rate_limit_ban_until_ns(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  return JS_NewInt64(ctx, flox_rate_limit_policy_ban_until_ns(
                              static_cast<FloxRateLimitPolicyHandle>(
                                  getHandle(ctx, argv[0]))));
}
static JSValue js_rate_limit_consecutive_rejects(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  return JS_NewUint32(ctx, flox_rate_limit_policy_consecutive_rejects(
                               static_cast<FloxRateLimitPolicyHandle>(
                                   getHandle(ctx, argv[0]))));
}
static JSValue js_executor_set_rate_limit_policy(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  flox_simulated_executor_set_rate_limit_policy(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<FloxRateLimitPolicyHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_clear_rate_limit_policy(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  flox_simulated_executor_clear_rate_limit_policy(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

// Venue availability.
static JSValue js_venue_availability_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, flox_venue_availability_create());
}
static JSValue js_venue_availability_destroy(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  flox_venue_availability_destroy(
      static_cast<FloxVenueAvailabilityHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_venue_availability_schedule_outage(JSContext* ctx, JSValueConst, int argc,
                                                     JSValueConst* argv)
{
  flox_venue_availability_schedule_outage(
      static_cast<FloxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), toInt64(ctx, argv[2]),
      argc > 3 ? static_cast<uint8_t>(toInt64(ctx, argv[3])) : 0,
      argc > 4 ? toInt64(ctx, argv[4]) : 0);
  return JS_UNDEFINED;
}
static JSValue js_venue_availability_auto_random_outages(JSContext* ctx, JSValueConst,
                                                         int argc, JSValueConst* argv)
{
  flox_venue_availability_auto_random_outages(
      static_cast<FloxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
      toDouble(ctx, argv[1]), toInt64(ctx, argv[2]),
      argc > 3 ? static_cast<uint8_t>(toInt64(ctx, argv[3])) : 0,
      argc > 4 ? static_cast<uint64_t>(toInt64(ctx, argv[4])) : 0xC0FFEEULL);
  return JS_UNDEFINED;
}
static JSValue js_venue_availability_is_up(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  const uint8_t up = flox_venue_availability_is_up(
      static_cast<FloxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]));
  return JS_NewBool(ctx, up != 0);
}
static JSValue js_executor_set_venue_availability(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  flox_simulated_executor_set_venue_availability(
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<FloxVenueAvailabilityHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}

// Fee schedule.
static JSValue js_fee_schedule_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, flox_fee_schedule_create());
}
static JSValue js_fee_schedule_destroy(JSContext* ctx, JSValueConst, int,
                                       JSValueConst* argv)
{
  flox_fee_schedule_destroy(
      static_cast<FloxFeeScheduleHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_fee_schedule_add_tier(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  double min_notional = 0.0, maker = 0.0, taker = 0.0;
  JS_ToFloat64(ctx, &min_notional, argv[1]);
  JS_ToFloat64(ctx, &maker, argv[2]);
  JS_ToFloat64(ctx, &taker, argv[3]);
  flox_fee_schedule_add_tier(
      static_cast<FloxFeeScheduleHandle>(getHandle(ctx, argv[0])), min_notional, maker,
      taker);
  return JS_UNDEFINED;
}
static JSValue js_fee_schedule_load_profile(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  const char* name = JS_ToCString(ctx, argv[1]);
  flox_fee_schedule_load_profile(
      static_cast<FloxFeeScheduleHandle>(getHandle(ctx, argv[0])), name);
  if (name)
  {
    JS_FreeCString(ctx, name);
  }
  return JS_UNDEFINED;
}
// Liquidation engine.
static JSValue js_liquidation_engine_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, flox_liquidation_engine_create());
}
static JSValue js_liquidation_engine_destroy(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  flox_liquidation_engine_destroy(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_add_tier(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  flox_liquidation_engine_add_tier(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      toDouble(ctx, argv[1]), toDouble(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_set_insurance_fund_capital(
    JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_liquidation_engine_set_insurance_fund_capital(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      toDouble(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_insurance_fund_balance(
    JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, flox_liquidation_engine_insurance_fund_balance(
               static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_liquidation_engine_set_adl_enabled(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  flox_liquidation_engine_set_adl_enabled(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      static_cast<uint8_t>(toUint32(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_set_adl_ranking(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  uint8_t code = 0;
  if (JS_IsString(argv[1]))
  {
    const char* s = JS_ToCString(ctx, argv[1]);
    if (s != nullptr)
    {
      std::string name(s);
      JS_FreeCString(ctx, s);
      if (name == "binance")
      {
        code = 1;
      }
      else if (name == "bybit")
      {
        code = 2;
      }
      else if (name == "position_size")
      {
        code = 3;
      }
    }
  }
  else
  {
    code = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  }
  flox_liquidation_engine_set_adl_ranking(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])), code);
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_adl_ranking(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  const uint8_t code = flox_liquidation_engine_adl_ranking(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])));
  return JS_NewUint32(ctx, code);
}
static JSValue js_liquidation_engine_set_liquidation_slippage_bps(
    JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_liquidation_engine_set_liquidation_slippage_bps(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      toDouble(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_open_position(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  flox_liquidation_engine_open_position(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])),
      toUint32(ctx, argv[2]), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), toDouble(ctx, argv[5]));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_close_position(JSContext* ctx, JSValueConst, int,
                                                    JSValueConst* argv)
{
  flox_liquidation_engine_close_position(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])),
      toUint32(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_on_mark(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  return JS_NewUint32(ctx, flox_liquidation_engine_on_mark(
                               static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
                               toUint32(ctx, argv[1]), toDouble(ctx, argv[2])));
}
static JSValue js_liquidation_engine_liquidations_count(JSContext* ctx, JSValueConst, int,
                                                        JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, static_cast<double>(flox_liquidation_engine_liquidations_count(
               static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])))));
}
static JSValue js_liquidation_engine_insurance_payments_count(JSContext* ctx, JSValueConst, int,
                                                              JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, static_cast<double>(flox_liquidation_engine_insurance_payments_count(
               static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])))));
}
static JSValue js_liquidation_engine_adl_closeouts_count(JSContext* ctx, JSValueConst, int,
                                                         JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, static_cast<double>(flox_liquidation_engine_adl_closeouts_count(
               static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])))));
}
static JSValue js_liquidation_engine_load_profile(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  flox_liquidation_engine_load_profile(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      static_cast<uint8_t>(toUint32(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_set_executor(JSContext* ctx, JSValueConst, int argc,
                                                  JSValueConst* argv)
{
  FloxSimulatedExecutorHandle exec_h = nullptr;
  if (argc >= 2 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
  {
    exec_h = static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[1]));
  }
  flox_liquidation_engine_set_executor(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])), exec_h);
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_ticks_to_first_adl(JSContext* ctx, JSValueConst, int,
                                                        JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, static_cast<double>(flox_liquidation_engine_ticks_to_first_adl(
               static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])))));
}
static JSValue js_liquidation_engine_reset_stats(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  flox_liquidation_engine_reset_stats(
      static_cast<FloxLiquidationEngineHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
template <typename T, typename SizeFn, typename CopyFn>
static JSValue copyVecToJs(JSContext* ctx, void* handle, SizeFn sizeFn, CopyFn copyFn)
{
  const uint32_t n = sizeFn(handle);
  std::vector<T> buf(n);
  copyFn(handle, buf.data(), n);
  JSValue arr = JS_NewArray(ctx);
  for (uint32_t i = 0; i < n; ++i)
  {
    JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, static_cast<double>(buf[i])));
  }
  return arr;
}
static JSValue js_liquidation_engine_deficits_paid_by_fund(JSContext* ctx, JSValueConst, int,
                                                           JSValueConst* argv)
{
  return copyVecToJs<double>(
      ctx, getHandle(ctx, argv[0]),
      [](void* h)
      { return flox_liquidation_engine_deficits_paid_by_fund_size(
            static_cast<FloxLiquidationEngineHandle>(h)); },
      [](void* h, double* o, uint32_t n)
      {
        flox_liquidation_engine_deficits_paid_by_fund_copy(
            static_cast<FloxLiquidationEngineHandle>(h), o, n);
      });
}
static JSValue js_liquidation_engine_deficits_paid_by_adl(JSContext* ctx, JSValueConst, int,
                                                          JSValueConst* argv)
{
  return copyVecToJs<double>(
      ctx, getHandle(ctx, argv[0]),
      [](void* h)
      { return flox_liquidation_engine_deficits_paid_by_adl_size(
            static_cast<FloxLiquidationEngineHandle>(h)); },
      [](void* h, double* o, uint32_t n)
      {
        flox_liquidation_engine_deficits_paid_by_adl_copy(
            static_cast<FloxLiquidationEngineHandle>(h), o, n);
      });
}
static JSValue js_liquidation_engine_cascade_sizes(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  return copyVecToJs<uint32_t>(
      ctx, getHandle(ctx, argv[0]),
      [](void* h)
      { return flox_liquidation_engine_cascade_sizes_size(
            static_cast<FloxLiquidationEngineHandle>(h)); },
      [](void* h, uint32_t* o, uint32_t n)
      {
        flox_liquidation_engine_cascade_sizes_copy(
            static_cast<FloxLiquidationEngineHandle>(h), o, n);
      });
}
static JSValue js_liquidation_engine_fund_balance_history(JSContext* ctx, JSValueConst, int,
                                                          JSValueConst* argv)
{
  return copyVecToJs<double>(
      ctx, getHandle(ctx, argv[0]),
      [](void* h)
      { return flox_liquidation_engine_fund_balance_history_size(
            static_cast<FloxLiquidationEngineHandle>(h)); },
      [](void* h, double* o, uint32_t n)
      {
        flox_liquidation_engine_fund_balance_history_copy(
            static_cast<FloxLiquidationEngineHandle>(h), o, n);
      });
}

static JSValue js_fee_schedule_record_fill(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  double notional = 0.0;
  JS_ToFloat64(ctx, &notional, argv[2]);
  flox_fee_schedule_record_fill(
      static_cast<FloxFeeScheduleHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), notional);
  return JS_UNDEFINED;
}
static JSValue js_fee_schedule_fee_for(JSContext* ctx, JSValueConst, int,
                                       JSValueConst* argv)
{
  double notional = 0.0;
  JS_ToFloat64(ctx, &notional, argv[2]);
  uint8_t is_maker = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  return JS_NewFloat64(ctx, flox_fee_schedule_fee_for(
                                static_cast<FloxFeeScheduleHandle>(
                                    getHandle(ctx, argv[0])),
                                toInt64(ctx, argv[1]), notional, is_maker));
}
static JSValue js_fee_schedule_current_tier(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  return JS_NewUint32(ctx, flox_fee_schedule_current_tier(
                               static_cast<FloxFeeScheduleHandle>(
                                   getHandle(ctx, argv[0]))));
}
static JSValue js_fee_schedule_rolling_notional(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  return JS_NewFloat64(ctx, flox_fee_schedule_rolling_notional(
                                static_cast<FloxFeeScheduleHandle>(
                                    getHandle(ctx, argv[0]))));
}
static JSValue js_fee_schedule_reset(JSContext* ctx, JSValueConst, int,
                                     JSValueConst* argv)
{
  flox_fee_schedule_reset_rolling(
      static_cast<FloxFeeScheduleHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

// Funding schedule.
static JSValue js_funding_schedule_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, flox_funding_schedule_create());
}
static JSValue js_funding_schedule_destroy(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  flox_funding_schedule_destroy(
      static_cast<FloxFundingScheduleHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_funding_schedule_set_constant(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  auto h = static_cast<FloxFundingScheduleHandle>(getHandle(ctx, argv[0]));
  int64_t interval = toInt64(ctx, argv[1]);
  double rate = 0.0;
  JS_ToFloat64(ctx, &rate, argv[2]);
  flox_funding_schedule_set_constant(h, interval, rate);
  return JS_UNDEFINED;
}
static JSValue js_funding_schedule_load_profile(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  auto h = static_cast<FloxFundingScheduleHandle>(getHandle(ctx, argv[0]));
  const char* name = JS_ToCString(ctx, argv[1]);
  flox_funding_schedule_load_profile(h, name);
  if (name)
  {
    JS_FreeCString(ctx, name);
  }
  return JS_UNDEFINED;
}
static JSValue js_funding_schedule_set_constant_rate(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  auto h = static_cast<FloxFundingScheduleHandle>(getHandle(ctx, argv[0]));
  double rate = 0.0;
  JS_ToFloat64(ctx, &rate, argv[1]);
  flox_funding_schedule_set_constant_rate(h, rate);
  return JS_UNDEFINED;
}
static JSValue js_funding_schedule_reset(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  flox_funding_schedule_reset(
      static_cast<FloxFundingScheduleHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_funding_schedule_tick(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  auto h = static_cast<FloxFundingScheduleHandle>(getHandle(ctx, argv[0]));
  int64_t nowNs = toInt64(ctx, argv[1]);
  auto syms = argv[2];
  auto pos = argv[3];
  auto mk = argv[4];
  uint32_t n = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, syms, "length");
  JS_ToUint32(ctx, &n, lenVal);
  JS_FreeValue(ctx, lenVal);
  std::vector<uint32_t> sy(n);
  std::vector<double> ps(n);
  std::vector<double> mp(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, syms, i);
    sy[i] = toUint32(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyUint32(ctx, pos, i);
    double d = 0.0;
    JS_ToFloat64(ctx, &d, v);
    ps[i] = d;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyUint32(ctx, mk, i);
    d = 0.0;
    JS_ToFloat64(ctx, &d, v);
    mp[i] = d;
    JS_FreeValue(ctx, v);
  }
  uint32_t count =
      flox_funding_schedule_tick(h, nowNs, sy.data(), ps.data(), mp.data(), n, nullptr, 0);
  std::vector<double> buf(count * 6, 0.0);
  flox_funding_schedule_tick(h, nowNs, sy.data(), ps.data(), mp.data(), n, buf.data(),
                             count);
  JSValue arr = JS_NewArray(ctx);
  for (uint32_t i = 0; i < count; ++i)
  {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "timestampNs",
                      JS_NewInt64(ctx, static_cast<int64_t>(buf[i * 6 + 0])));
    JS_SetPropertyStr(ctx, obj, "symbol", JS_NewFloat64(ctx, buf[i * 6 + 1]));
    JS_SetPropertyStr(ctx, obj, "rate", JS_NewFloat64(ctx, buf[i * 6 + 2]));
    JS_SetPropertyStr(ctx, obj, "markPrice", JS_NewFloat64(ctx, buf[i * 6 + 3]));
    JS_SetPropertyStr(ctx, obj, "positionSigned", JS_NewFloat64(ctx, buf[i * 6 + 4]));
    JS_SetPropertyStr(ctx, obj, "amount", JS_NewFloat64(ctx, buf[i * 6 + 5]));
    JS_SetPropertyUint32(ctx, arr, i, obj);
  }
  return arr;
}
static JSValue js_executor_on_trade_qty(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  flox_simulated_executor_on_trade_qty(static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                       toUint32(ctx, argv[1]), toDouble(ctx, argv[2]),
                                       toDouble(ctx, argv[3]),
                                       static_cast<uint8_t>(toUint32(ctx, argv[4])));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_best_levels(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  flox_simulated_executor_on_best_levels(static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
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
      static_cast<FloxSimulatedExecutorHandle>(getHandle(ctx, argv[1])));
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
// Delta book compression bindings
// ============================================================

static std::vector<FloxBookLevel> readDeltaBookLevels(JSContext* ctx, JSValueConst arr)
{
  std::vector<FloxBookLevel> out;
  if (!JS_IsArray(ctx, arr))
  {
    return out;
  }
  uint32_t len = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
  JS_ToUint32(ctx, &len, lenVal);
  JS_FreeValue(ctx, lenVal);
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i)
  {
    JSValue entry = JS_GetPropertyUint32(ctx, arr, i);
    FloxBookLevel l{};
    JSValue p = JS_GetPropertyStr(ctx, entry, "priceRaw");
    JSValue q = JS_GetPropertyStr(ctx, entry, "qtyRaw");
    l.price_raw = toInt64(ctx, p);
    l.quantity_raw = toInt64(ctx, q);
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, q);
    JS_FreeValue(ctx, entry);
    out.push_back(l);
  }
  return out;
}

static JSValue deltaBookLevelsToJs(JSContext* ctx, const std::vector<FloxBookLevel>& levels)
{
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < levels.size(); ++i)
  {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "priceRaw", JS_NewInt64(ctx, levels[i].price_raw));
    JS_SetPropertyStr(ctx, o, "qtyRaw", JS_NewInt64(ctx, levels[i].quantity_raw));
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

static JSValue js_delta_book_encoder_create(JSContext* ctx, JSValueConst, int argc,
                                            JSValueConst* argv)
{
  uint32_t anchor_every = (argc > 0) ? toUint32(ctx, argv[0]) : 100;
  auto h = flox_delta_book_encoder_create(anchor_every);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "DeltaBookEncoder: construction failed");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_delta_book_encoder_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_delta_book_encoder_destroy(
      static_cast<FloxDeltaBookEncoderHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_delta_book_encoder_encode(JSContext* ctx, JSValueConst, int argc,
                                            JSValueConst* argv)
{
  if (argc < 4)
  {
    return JS_ThrowTypeError(ctx, "encode(handle, symbol, bids, asks)");
  }
  auto h = static_cast<FloxDeltaBookEncoderHandle>(getHandle(ctx, argv[0]));
  uint32_t sym = toUint32(ctx, argv[1]);
  auto bids = readDeltaBookLevels(ctx, argv[2]);
  auto asks = readDeltaBookLevels(ctx, argv[3]);
  uint8_t is_delta = 0;
  uint64_t bcnt = 0, acnt = 0;
  flox_delta_book_encoder_encode(h, sym,
                                 bids.empty() ? nullptr : bids.data(), bids.size(),
                                 asks.empty() ? nullptr : asks.data(), asks.size(),
                                 &is_delta, &bcnt, &acnt);
  std::vector<FloxBookLevel> out_bids(bcnt), out_asks(acnt);
  if (bcnt)
  {
    flox_delta_book_encoder_copy_bids(h, out_bids.data(), bcnt);
  }
  if (acnt)
  {
    flox_delta_book_encoder_copy_asks(h, out_asks.data(), acnt);
  }
  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "isDelta", JS_NewBool(ctx, is_delta != 0));
  JS_SetPropertyStr(ctx, out, "bids", deltaBookLevelsToJs(ctx, out_bids));
  JS_SetPropertyStr(ctx, out, "asks", deltaBookLevelsToJs(ctx, out_asks));
  return out;
}

static JSValue js_delta_book_replayer_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, flox_delta_book_replayer_create());
}

static JSValue js_delta_book_replayer_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_delta_book_replayer_destroy(
      static_cast<FloxDeltaBookReplayerHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_delta_book_replayer_apply(JSContext* ctx, JSValueConst, int argc,
                                            JSValueConst* argv)
{
  if (argc < 5)
  {
    return JS_ThrowTypeError(ctx, "apply(handle, type, symbol, bids, asks)");
  }
  auto h = static_cast<FloxDeltaBookReplayerHandle>(getHandle(ctx, argv[0]));
  uint8_t type = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  uint32_t sym = toUint32(ctx, argv[2]);
  auto bids = readDeltaBookLevels(ctx, argv[3]);
  auto asks = readDeltaBookLevels(ctx, argv[4]);
  uint64_t bcnt = 0, acnt = 0;
  flox_delta_book_replayer_apply(h, type, sym,
                                 bids.empty() ? nullptr : bids.data(), bids.size(),
                                 asks.empty() ? nullptr : asks.data(), asks.size(),
                                 &bcnt, &acnt);
  std::vector<FloxBookLevel> out_bids(bcnt), out_asks(acnt);
  if (bcnt)
  {
    flox_delta_book_replayer_copy_bids(h, out_bids.data(), bcnt);
  }
  if (acnt)
  {
    flox_delta_book_replayer_copy_asks(h, out_asks.data(), acnt);
  }
  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "bids", deltaBookLevelsToJs(ctx, out_bids));
  JS_SetPropertyStr(ctx, out, "asks", deltaBookLevelsToJs(ctx, out_asks));
  return out;
}

// ============================================================
// .floxrun trace recorder / reader bindings
// ============================================================

static std::vector<uint32_t> readU32Array(JSContext* ctx, JSValueConst arr)
{
  std::vector<uint32_t> out;
  if (!JS_IsArray(ctx, arr))
  {
    return out;
  }
  uint32_t len = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
  JS_ToUint32(ctx, &len, lenVal);
  JS_FreeValue(ctx, lenVal);
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, arr, i);
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    out.push_back(n);
  }
  return out;
}

static JSValue u32VecToJs(JSContext* ctx, const std::vector<uint32_t>& v)
{
  JSValue out = JS_NewArray(ctx);
  for (size_t i = 0; i < v.size(); ++i)
  {
    JS_SetPropertyUint32(ctx, out, static_cast<uint32_t>(i),
                         JS_NewUint32(ctx, v[i]));
  }
  return out;
}

static JSValue js_run_recorder_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 1)
  {
    return JS_ThrowTypeError(ctx, "TraceRecorder: need options object");
  }
  auto getStr = [&](const char* key, const char* dflt = "") -> std::string
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
    if (JS_IsUndefined(v) || JS_IsNull(v))
    {
      JS_FreeValue(ctx, v);
      return dflt;
    }
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    if (s)
    {
      JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  auto getI64 = [&](const char* key, int64_t dflt = 0) -> int64_t
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
    int64_t out = dflt;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToInt64(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  std::string path = getStr("path");
  std::string sid = getStr("strategyId");
  std::string shash = getStr("strategyHash");
  int64_t started = getI64("runStartedNs");
  auto h = flox_run_recorder_create(path.c_str(), sid.c_str(), shash.c_str(), started);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "TraceRecorder: construction failed");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_run_recorder_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_run_recorder_destroy(static_cast<FloxRunRecorderHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_add_tape_ref(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 2)
  {
    return JS_ThrowTypeError(ctx, "addTapeRef(handle, opts)");
  }
  auto h = static_cast<FloxRunRecorderHandle>(getHandle(ctx, argv[0]));
  auto getStr = [&](const char* key) -> std::string
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[1], key);
    if (JS_IsUndefined(v) || JS_IsNull(v))
    {
      JS_FreeValue(ctx, v);
      return "";
    }
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    if (s)
    {
      JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  auto getI64 = [&](const char* key) -> int64_t
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[1], key);
    int64_t out = 0;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToInt64(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  std::string path = getStr("path");
  std::string ch = getStr("contentHash");
  flox_run_recorder_add_tape_ref(h, path.c_str(), ch.c_str(), getI64("firstEventNs"),
                                 getI64("lastEventNs"));
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_set_run_ended_ns(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxRunRecorderHandle>(getHandle(ctx, argv[0]));
  flox_run_recorder_set_run_ended_ns(h, toInt64(ctx, argv[1]));
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_write_signal(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 2)
  {
    return JS_ThrowTypeError(ctx, "writeSignal(handle, opts)");
  }
  auto h = static_cast<FloxRunRecorderHandle>(getHandle(ctx, argv[0]));
  auto opts = argv[1];
  auto getI64 = [&](const char* k, int64_t d = 0) -> int64_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    int64_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToInt64(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  auto getU32 = [&](const char* k, uint32_t d = 0) -> uint32_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    uint32_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToUint32(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  int64_t run_ts = getI64("runTsNs");
  int64_t feed_ts = getI64("feedTsNs");
  uint32_t signal_id = getU32("signalId");
  uint32_t flags = getU32("flags");
  int64_t strength = getI64("strengthRaw");
  std::string name;
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, "name");
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      const char* s = JS_ToCString(ctx, v);
      if (s)
      {
        name = s;
        JS_FreeCString(ctx, s);
      }
    }
    JS_FreeValue(ctx, v);
  }
  std::vector<uint32_t> sids;
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, "symbolIds");
    if (JS_IsArray(ctx, v))
    {
      sids = readU32Array(ctx, v);
    }
    JS_FreeValue(ctx, v);
  }
  std::vector<uint8_t> payload;
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, "payload");
    if (JS_IsString(v))
    {
      const char* s = JS_ToCString(ctx, v);
      if (s)
      {
        payload.assign(s, s + std::strlen(s));
        JS_FreeCString(ctx, s);
      }
    }
    JS_FreeValue(ctx, v);
  }
  flox_run_recorder_write_signal(h, run_ts, feed_ts, signal_id, flags, strength,
                                 name.empty() ? nullptr : name.data(), name.size(),
                                 sids.empty() ? nullptr : sids.data(), sids.size(),
                                 payload.empty() ? nullptr : payload.data(), payload.size());
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_write_order_event(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 2)
  {
    return JS_ThrowTypeError(ctx, "writeOrderEvent(handle, opts)");
  }
  auto h = static_cast<FloxRunRecorderHandle>(getHandle(ctx, argv[0]));
  auto opts = argv[1];
  auto getI64 = [&](const char* k, int64_t d = 0) -> int64_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    int64_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToInt64(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  auto getU32 = [&](const char* k, uint32_t d = 0) -> uint32_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    uint32_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToUint32(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  std::string reason;
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, "reason");
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      const char* s = JS_ToCString(ctx, v);
      if (s)
      {
        reason = s;
        JS_FreeCString(ctx, s);
      }
    }
    JS_FreeValue(ctx, v);
  }
  flox_run_recorder_write_order_event(h, getI64("runTsNs"), getI64("feedTsNs"),
                                      static_cast<uint64_t>(getI64("orderId")),
                                      static_cast<uint64_t>(getI64("parentSignalId")),
                                      getI64("priceRaw"), getI64("qtyRaw"),
                                      getU32("symbolId"),
                                      static_cast<uint8_t>(getU32("eventKind", 1)),
                                      static_cast<uint8_t>(getU32("side")),
                                      static_cast<uint8_t>(getU32("orderType")),
                                      getU32("flags"),
                                      reason.empty() ? nullptr : reason.data(), reason.size());
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_write_fill(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 2)
  {
    return JS_ThrowTypeError(ctx, "writeFill(handle, opts)");
  }
  auto h = static_cast<FloxRunRecorderHandle>(getHandle(ctx, argv[0]));
  auto opts = argv[1];
  auto getI64 = [&](const char* k, int64_t d = 0) -> int64_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    int64_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToInt64(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  auto getU32 = [&](const char* k, uint32_t d = 0) -> uint32_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    uint32_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToUint32(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  flox_run_recorder_write_fill(h, getI64("runTsNs"), getI64("feedTsNs"),
                               static_cast<uint64_t>(getI64("orderId")),
                               static_cast<uint64_t>(getI64("fillId")),
                               getI64("priceRaw"), getI64("qtyRaw"), getI64("feeRaw"),
                               getU32("symbolId"),
                               static_cast<uint8_t>(getU32("side")),
                               static_cast<uint8_t>(getU32("liquidity")));
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_close(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_run_recorder_close(static_cast<FloxRunRecorderHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_run_reader_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 1)
  {
    return JS_ThrowTypeError(ctx, "TraceReader(path)");
  }
  const char* path = JS_ToCString(ctx, argv[0]);
  auto h = flox_run_reader_open(path ? path : "");
  if (path)
  {
    JS_FreeCString(ctx, path);
  }
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "TraceReader: cannot open path");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_run_reader_close(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_run_reader_close(static_cast<FloxRunReaderHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_run_reader_strategy_id(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxRunReaderHandle>(getHandle(ctx, argv[0]));
  uint64_t n = flox_run_reader_strategy_id(h, nullptr, 0);
  std::string out(n, '\0');
  if (n)
  {
    flox_run_reader_strategy_id(h, out.data(), n);
  }
  return JS_NewStringLen(ctx, out.data(), out.size());
}

static JSValue js_run_reader_run_started_ns(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxRunReaderHandle>(getHandle(ctx, argv[0]));
  return JS_NewInt64(ctx, flox_run_reader_run_started_ns(h));
}

static JSValue js_run_reader_run_ended_ns(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxRunReaderHandle>(getHandle(ctx, argv[0]));
  return JS_NewInt64(ctx, flox_run_reader_run_ended_ns(h));
}

static JSValue js_run_reader_signals(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxRunReaderHandle>(getHandle(ctx, argv[0]));
  uint64_t n = flox_run_reader_signal_count(h);
  JSValue out = JS_NewArray(ctx);
  for (uint64_t i = 0; i < n; ++i)
  {
    int64_t run_ts = 0, feed_ts = 0, strength = 0;
    uint32_t sid = 0, flags = 0;
    uint64_t name_len = 0, sym_count = 0, payload_len = 0;
    flox_run_reader_signal_header(h, i, &run_ts, &feed_ts, &sid, &flags, &strength, &name_len,
                                  &sym_count, &payload_len);
    std::string name(name_len, '\0');
    if (name_len)
    {
      flox_run_reader_signal_name(h, i, name.data(), name_len);
    }
    std::vector<uint32_t> sids(sym_count);
    if (sym_count)
    {
      flox_run_reader_signal_symbol_ids(h, i, sids.data(), sym_count);
    }
    std::vector<uint8_t> payload(payload_len);
    if (payload_len)
    {
      flox_run_reader_signal_payload(h, i, payload.data(), payload_len);
    }
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "runTsNs", JS_NewInt64(ctx, run_ts));
    JS_SetPropertyStr(ctx, rec, "feedTsNs", JS_NewInt64(ctx, feed_ts));
    JS_SetPropertyStr(ctx, rec, "signalId", JS_NewUint32(ctx, sid));
    JS_SetPropertyStr(ctx, rec, "flags", JS_NewUint32(ctx, flags));
    JS_SetPropertyStr(ctx, rec, "strengthRaw", JS_NewInt64(ctx, strength));
    JS_SetPropertyStr(ctx, rec, "name", JS_NewStringLen(ctx, name.data(), name.size()));
    JS_SetPropertyStr(ctx, rec, "symbolIds", u32VecToJs(ctx, sids));
    JS_SetPropertyStr(ctx, rec, "payload",
                      JS_NewStringLen(ctx, reinterpret_cast<const char*>(payload.data()),
                                      payload.size()));
    JS_SetPropertyUint32(ctx, out, static_cast<uint32_t>(i), rec);
  }
  return out;
}

static JSValue js_run_reader_orders(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxRunReaderHandle>(getHandle(ctx, argv[0]));
  uint64_t n = flox_run_reader_order_event_count(h);
  JSValue out = JS_NewArray(ctx);
  for (uint64_t i = 0; i < n; ++i)
  {
    int64_t run_ts = 0, feed_ts = 0, price = 0, qty = 0;
    uint64_t oid = 0, pid = 0, reason_len = 0;
    uint32_t sid = 0, flags = 0;
    uint8_t kind = 0, side = 0, otype = 0;
    flox_run_reader_order_event_header(h, i, &run_ts, &feed_ts, &oid, &pid, &price, &qty, &sid,
                                       &kind, &side, &otype, &flags, &reason_len);
    std::string reason(reason_len, '\0');
    if (reason_len)
    {
      flox_run_reader_order_event_reason(h, i, reason.data(), reason_len);
    }
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "runTsNs", JS_NewInt64(ctx, run_ts));
    JS_SetPropertyStr(ctx, rec, "feedTsNs", JS_NewInt64(ctx, feed_ts));
    JS_SetPropertyStr(ctx, rec, "orderId", JS_NewInt64(ctx, static_cast<int64_t>(oid)));
    JS_SetPropertyStr(ctx, rec, "parentSignalId", JS_NewInt64(ctx, static_cast<int64_t>(pid)));
    JS_SetPropertyStr(ctx, rec, "priceRaw", JS_NewInt64(ctx, price));
    JS_SetPropertyStr(ctx, rec, "qtyRaw", JS_NewInt64(ctx, qty));
    JS_SetPropertyStr(ctx, rec, "symbolId", JS_NewUint32(ctx, sid));
    JS_SetPropertyStr(ctx, rec, "eventKind", JS_NewUint32(ctx, kind));
    JS_SetPropertyStr(ctx, rec, "side", JS_NewUint32(ctx, side));
    JS_SetPropertyStr(ctx, rec, "orderType", JS_NewUint32(ctx, otype));
    JS_SetPropertyStr(ctx, rec, "flags", JS_NewUint32(ctx, flags));
    JS_SetPropertyStr(ctx, rec, "reason", JS_NewStringLen(ctx, reason.data(), reason.size()));
    JS_SetPropertyUint32(ctx, out, static_cast<uint32_t>(i), rec);
  }
  return out;
}

static JSValue js_run_reader_fills(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxRunReaderHandle>(getHandle(ctx, argv[0]));
  uint64_t n = flox_run_reader_fill_count(h);
  JSValue out = JS_NewArray(ctx);
  for (uint64_t i = 0; i < n; ++i)
  {
    int64_t run_ts = 0, feed_ts = 0, price = 0, qty = 0, fee = 0;
    uint64_t oid = 0, fid = 0;
    uint32_t sid = 0;
    uint8_t side = 0, liq = 0;
    flox_run_reader_fill(h, i, &run_ts, &feed_ts, &oid, &fid, &price, &qty, &fee, &sid, &side, &liq);
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "runTsNs", JS_NewInt64(ctx, run_ts));
    JS_SetPropertyStr(ctx, rec, "feedTsNs", JS_NewInt64(ctx, feed_ts));
    JS_SetPropertyStr(ctx, rec, "orderId", JS_NewInt64(ctx, static_cast<int64_t>(oid)));
    JS_SetPropertyStr(ctx, rec, "fillId", JS_NewInt64(ctx, static_cast<int64_t>(fid)));
    JS_SetPropertyStr(ctx, rec, "priceRaw", JS_NewInt64(ctx, price));
    JS_SetPropertyStr(ctx, rec, "qtyRaw", JS_NewInt64(ctx, qty));
    JS_SetPropertyStr(ctx, rec, "feeRaw", JS_NewInt64(ctx, fee));
    JS_SetPropertyStr(ctx, rec, "symbolId", JS_NewUint32(ctx, sid));
    JS_SetPropertyStr(ctx, rec, "side", JS_NewUint32(ctx, side));
    JS_SetPropertyStr(ctx, rec, "liquidity", JS_NewUint32(ctx, liq));
    JS_SetPropertyUint32(ctx, out, static_cast<uint32_t>(i), rec);
  }
  return out;
}

// ============================================================
// Order group (multi-leg state machine + risk gate + pair-latency)
// ============================================================
//
// Thin shim over the C ABI. The high-level OrderGroup class lives in
// quickjs/flox/order_group.js and wraps these globals so the four
// bindings (pybind11 / NAPI / QuickJS / Codon) all reach the same
// C++ engine.

static JSValue js_order_group_create(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  int64_t parent = 0;
  uint32_t policy = 0;
  if (argv)
  {
    JS_ToInt64(ctx, &parent, argv[0]);
    JS_ToUint32(ctx, &policy, argv[1]);
  }
  auto h = flox_order_group_create(static_cast<uint64_t>(parent), static_cast<uint8_t>(policy));
  return createHandleObject(ctx, h);
}

static JSValue js_order_group_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_order_group_destroy(static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_order_group_add_market_leg(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  double qty = toDouble(ctx, argv[3]);
  int64_t qty_raw = static_cast<int64_t>(qty * 100'000'000LL);
  return JS_NewUint32(ctx,
                      flox_order_group_add_market_leg(h, symbol, static_cast<uint8_t>(side),
                                                      qty_raw));
}

static JSValue js_order_group_add_limit_leg(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  double price = toDouble(ctx, argv[3]);
  double qty = toDouble(ctx, argv[4]);
  int64_t price_raw = static_cast<int64_t>(price * 100'000'000LL);
  int64_t qty_raw = static_cast<int64_t>(qty * 100'000'000LL);
  return JS_NewUint32(
      ctx, flox_order_group_add_limit_leg(h, symbol, static_cast<uint8_t>(side), price_raw,
                                          qty_raw));
}

static JSValue js_order_group_leg_count(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  return JS_NewUint32(ctx, flox_order_group_leg_count(h));
}

static JSValue js_order_group_leg_state(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  return JS_NewUint32(ctx, flox_order_group_leg_state(h, i));
}

static JSValue js_order_group_leg_filled(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  int64_t raw = flox_order_group_leg_filled_raw(h, i);
  return JS_NewFloat64(ctx, static_cast<double>(raw) / 1e8);
}

static JSValue js_order_group_leg_order_id(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  return JS_NewInt64(ctx, static_cast<int64_t>(flox_order_group_leg_order_id(h, i)));
}

static JSValue js_order_group_record_submit(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  int64_t order_id = 0;
  JS_ToInt64(ctx, &order_id, argv[2]);
  flox_order_group_record_submit(h, i, static_cast<uint64_t>(order_id));
  return JS_UNDEFINED;
}

static JSValue js_order_group_record_fill(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  double qty = toDouble(ctx, argv[2]);
  flox_order_group_record_fill(h, i, static_cast<int64_t>(qty * 100'000'000LL));
  return JS_UNDEFINED;
}

static JSValue js_order_group_record_cancel(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  flox_order_group_record_cancel(h, i);
  return JS_UNDEFINED;
}

static JSValue js_order_group_record_failure(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  flox_order_group_record_failure(h, i);
  return JS_UNDEFINED;
}

static JSValue js_order_group_record_replace_accepted(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  uint64_t new_id = static_cast<uint64_t>(toInt64(ctx, argv[2]));
  flox_order_group_record_replace_accepted(h, i, new_id);
  return JS_UNDEFINED;
}

static JSValue js_order_group_record_replace_rejected(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  flox_order_group_record_replace_rejected(h, i);
  return JS_UNDEFINED;
}

static JSValue js_order_group_find_leg_by_order_id(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint64_t order_id = static_cast<uint64_t>(toInt64(ctx, argv[1]));
  uint32_t idx = flox_order_group_find_leg_by_order_id(h, order_id);
  if (idx == UINT32_MAX)
  {
    return JS_NULL;
  }
  return JS_NewUint32(ctx, idx);
}

static JSValue js_order_group_state(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  return JS_NewUint32(ctx, flox_order_group_state(h));
}

static JSValue js_order_group_recommended_actions(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  constexpr uint32_t kMax = 32;
  int64_t buf[kMax * 5];
  uint32_t n = flox_order_group_recommended_actions(h, buf, kMax);
  JSValue out = JS_NewArray(ctx);
  for (uint32_t i = 0; i < n; ++i)
  {
    int64_t* row = buf + i * 5;
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "kind",
                      JS_NewString(ctx, row[0] == 0 ? "cancel" : "revert"));
    JS_SetPropertyStr(ctx, rec, "legIndex", JS_NewInt32(ctx, static_cast<int32_t>(row[1])));
    if (row[0] == 0)
    {
      JS_SetPropertyStr(ctx, rec, "orderId", JS_NewInt64(ctx, row[2]));
    }
    else
    {
      JS_SetPropertyStr(ctx, rec, "symbol", JS_NewInt32(ctx, static_cast<int32_t>(row[2])));
      JS_SetPropertyStr(ctx, rec, "side", JS_NewInt32(ctx, static_cast<int32_t>(row[3])));
      JS_SetPropertyStr(ctx, rec, "qty", JS_NewFloat64(ctx, static_cast<double>(row[4]) / 1e8));
    }
    JS_SetPropertyUint32(ctx, out, i, rec);
  }
  return out;
}

static JSValue js_order_group_mark_action_dispatched(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t leg = toUint32(ctx, argv[1]);
  uint32_t kind = toUint32(ctx, argv[2]);
  flox_order_group_mark_action_dispatched(h, leg, static_cast<uint8_t>(kind));
  return JS_UNDEFINED;
}

static JSValue js_order_group_set_risk_limits(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  double max_gross = toDouble(ctx, argv[1]);
  double max_conc = toDouble(ctx, argv[2]);
  double max_leg = toDouble(ctx, argv[3]);
  int64_t gross_raw = static_cast<int64_t>(max_gross * 100'000'000LL);
  int64_t leg_raw = static_cast<int64_t>(max_leg * 100'000'000LL);
  flox_order_group_set_risk_limits(h, gross_raw, max_conc, leg_raw);
  return JS_UNDEFINED;
}

static JSValue js_order_group_precheck_submission(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  double equity = toDouble(ctx, argv[1]);

  std::vector<int64_t> prices_raw;
  uint32_t plen = 0;
  if (JS_IsArray(ctx, argv[2]))
  {
    JSValue lenVal = JS_GetPropertyStr(ctx, argv[2], "length");
    JS_ToUint32(ctx, &plen, lenVal);
    JS_FreeValue(ctx, lenVal);
    prices_raw.reserve(plen);
    for (uint32_t i = 0; i < plen; ++i)
    {
      JSValue v = JS_GetPropertyUint32(ctx, argv[2], i);
      double p = 0;
      JS_ToFloat64(ctx, &p, v);
      JS_FreeValue(ctx, v);
      prices_raw.push_back(static_cast<int64_t>(p * 100'000'000LL));
    }
  }

  char rule[64] = {};
  char detail[256] = {};
  uint8_t denied = flox_order_group_precheck_submission(
      h, equity, prices_raw.empty() ? nullptr : prices_raw.data(), plen, rule, sizeof(rule),
      detail, sizeof(detail));

  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "denied", JS_NewBool(ctx, denied != 0));
  JS_SetPropertyStr(ctx, out, "rule", JS_NewString(ctx, rule));
  JS_SetPropertyStr(ctx, out, "detail", JS_NewString(ctx, detail));
  return out;
}

static JSValue js_order_group_set_pair_latency_budget_ns(JSContext* ctx, JSValueConst, int,
                                                         JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  int64_t budget = 0;
  JS_ToInt64(ctx, &budget, argv[1]);
  flox_order_group_set_pair_latency_budget_ns(h, budget);
  return JS_UNDEFINED;
}

static JSValue js_order_group_pair_latency_decision(JSContext* ctx, JSValueConst, int,
                                                    JSValueConst* argv)
{
  auto h = static_cast<FloxOrderGroupHandle>(getHandle(ctx, argv[0]));
  int64_t submit_ts = 0, ack_ts = 0;
  JS_ToInt64(ctx, &submit_ts, argv[1]);
  JS_ToInt64(ctx, &ack_ts, argv[2]);
  uint32_t ack_received = toUint32(ctx, argv[3]);
  uint8_t d = flox_order_group_pair_latency_decision(h, submit_ts, ack_ts,
                                                     static_cast<uint8_t>(ack_received));
  const char* name = d == 0 ? "wait" : (d == 1 ? "submit_follower" : "cancel_leader");
  return JS_NewString(ctx, name);
}

// ============================================================
// Live queue position estimator
// ============================================================

static JSValue js_live_queue_position_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, flox_live_queue_position_create());
}

static JSValue js_live_queue_position_destroy(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  flox_live_queue_position_destroy(
      static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_set_half_life(JSContext* ctx, JSValueConst, int,
                                                    JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  flox_live_queue_position_set_confidence_half_life_ns(h, toInt64(ctx, argv[1]));
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_set_shrink_factor(JSContext* ctx, JSValueConst, int,
                                                        JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  double factor = 0.0;
  JS_ToFloat64(ctx, &factor, argv[1]);
  flox_live_queue_position_set_shrink_factor(h, factor);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_order_placed(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[2]));
  double price = 0.0, order_qty = 0.0, level_qty = 0.0;
  JS_ToFloat64(ctx, &price, argv[3]);
  uint64_t order_id = static_cast<uint64_t>(toInt64(ctx, argv[4]));
  JS_ToFloat64(ctx, &order_qty, argv[5]);
  JS_ToFloat64(ctx, &level_qty, argv[6]);
  int64_t ts_ns = toInt64(ctx, argv[7]);
  flox_live_queue_position_on_order_placed(
      h, symbol, side, flox_price_from_double(price), order_id,
      flox_quantity_from_double(order_qty), flox_quantity_from_double(level_qty),
      ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_order_cancelled(JSContext* ctx, JSValueConst,
                                                         int, JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint64_t order_id = static_cast<uint64_t>(toInt64(ctx, argv[1]));
  int64_t ts_ns = toInt64(ctx, argv[2]);
  flox_live_queue_position_on_order_cancelled(h, order_id, ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_order_filled(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint64_t order_id = static_cast<uint64_t>(toInt64(ctx, argv[1]));
  double cum = 0.0;
  JS_ToFloat64(ctx, &cum, argv[2]);
  int64_t ts_ns = toInt64(ctx, argv[3]);
  flox_live_queue_position_on_order_filled(h, order_id,
                                           flox_quantity_from_double(cum), ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_trade(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  double price = 0.0, qty = 0.0;
  JS_ToFloat64(ctx, &price, argv[2]);
  JS_ToFloat64(ctx, &qty, argv[3]);
  int64_t ts_ns = toInt64(ctx, argv[4]);
  flox_live_queue_position_on_trade(h, symbol, flox_price_from_double(price),
                                    flox_quantity_from_double(qty), ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_trade_with_flag(JSContext* ctx, JSValueConst,
                                                         int, JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  double price = 0.0, qty = 0.0;
  JS_ToFloat64(ctx, &price, argv[2]);
  JS_ToFloat64(ctx, &qty, argv[3]);
  int64_t ts_ns = toInt64(ctx, argv[4]);
  uint8_t is_hidden = static_cast<uint8_t>(toUint32(ctx, argv[5]));
  flox_live_queue_position_on_trade_with_flag(
      h, symbol, flox_price_from_double(price), flox_quantity_from_double(qty), ts_ns,
      is_hidden);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_set_hidden_order_policy(JSContext* ctx,
                                                              JSValueConst, int,
                                                              JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint8_t policy = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  flox_live_queue_position_set_hidden_order_policy(h, policy);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_level_update(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[2]));
  double price = 0.0, new_qty = 0.0;
  JS_ToFloat64(ctx, &price, argv[3]);
  JS_ToFloat64(ctx, &new_qty, argv[4]);
  int64_t ts_ns = toInt64(ctx, argv[5]);
  flox_live_queue_position_on_level_update(h, symbol, side,
                                           flox_price_from_double(price),
                                           flox_quantity_from_double(new_qty), ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_snapshot(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint64_t order_id = static_cast<uint64_t>(toInt64(ctx, argv[1]));
  int64_t now_ns = toInt64(ctx, argv[2]);
  int64_t slots[6] = {0};
  uint8_t ok = flox_live_queue_position_snapshot(h, order_id, now_ns, slots);
  if (!ok)
  {
    return JS_NULL;
  }
  double conf = 0.0;
  std::memcpy(&conf, &slots[4], sizeof(double));
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "orderId", JS_NewInt64(ctx, slots[0]));
  JS_SetPropertyStr(ctx, obj, "queueAheadEst",
                    JS_NewFloat64(ctx, flox_quantity_to_double(slots[1])));
  JS_SetPropertyStr(ctx, obj, "total",
                    JS_NewFloat64(ctx, flox_quantity_to_double(slots[2])));
  JS_SetPropertyStr(ctx, obj, "lastUpdateNs", JS_NewInt64(ctx, slots[3]));
  JS_SetPropertyStr(ctx, obj, "confidence", JS_NewFloat64(ctx, conf));
  JS_SetPropertyStr(ctx, obj, "hiddenVolumeSeen",
                    JS_NewFloat64(ctx, flox_quantity_to_double(slots[5])));
  return obj;
}

static JSValue js_live_queue_position_tracked_count(JSContext* ctx, JSValueConst, int,
                                                    JSValueConst* argv)
{
  auto h = static_cast<FloxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  return JS_NewUint32(ctx, flox_live_queue_position_tracked_count(h));
}

// ============================================================
// Bar dispatch recorder (cross-binding parity test fixture)
// ============================================================

static JSValue js_bar_dispatch_recorder_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, flox_bar_dispatch_recorder_create());
}

static JSValue js_bar_dispatch_recorder_destroy(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  flox_bar_dispatch_recorder_destroy(
      static_cast<FloxBarDispatchRecorderHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_bar_dispatch_recorder_add_time_seconds(JSContext* ctx, JSValueConst, int,
                                                         JSValueConst* argv)
{
  auto h = static_cast<FloxBarDispatchRecorderHandle>(getHandle(ctx, argv[0]));
  uint32_t seconds = toUint32(ctx, argv[1]);
  return JS_NewUint32(ctx, flox_bar_dispatch_recorder_add_time_seconds(h, seconds));
}

static JSValue js_bar_dispatch_recorder_on_trade(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  auto h = static_cast<FloxBarDispatchRecorderHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  double price = toDouble(ctx, argv[2]);
  double qty = toDouble(ctx, argv[3]);
  int64_t ts_ns = 0;
  JS_ToInt64(ctx, &ts_ns, argv[4]);
  flox_bar_dispatch_recorder_on_trade(h, symbol, price, qty, ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_bar_dispatch_recorder_finalize(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  flox_bar_dispatch_recorder_finalize(
      static_cast<FloxBarDispatchRecorderHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_bar_dispatch_recorder_count(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  auto h = static_cast<FloxBarDispatchRecorderHandle>(getHandle(ctx, argv[0]));
  return JS_NewUint32(ctx, flox_bar_dispatch_recorder_count(h));
}

static JSValue js_bar_dispatch_recorder_type_at(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  auto h = static_cast<FloxBarDispatchRecorderHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  return JS_NewUint32(ctx, flox_bar_dispatch_recorder_type_at(h, i));
}

static JSValue js_bar_dispatch_recorder_param_at(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  auto h = static_cast<FloxBarDispatchRecorderHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  return JS_NewInt64(ctx, static_cast<int64_t>(flox_bar_dispatch_recorder_param_at(h, i)));
}

// ============================================================
// Tape diff bindings (replay-equivalence localization)
// ============================================================

static JSValue js_tape_diff(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 2)
  {
    return JS_ThrowTypeError(ctx, "tapeDiff: need leftPath and rightPath");
  }
  const char* left = JS_ToCString(ctx, argv[0]);
  const char* right = JS_ToCString(ctx, argv[1]);
  if (!left || !right)
  {
    if (left)
    {
      JS_FreeCString(ctx, left);
    }
    if (right)
    {
      JS_FreeCString(ctx, right);
    }
    return JS_ThrowTypeError(ctx, "tapeDiff: paths must be strings");
  }
  uint32_t max_mismatches = 16;
  int64_t tolerance_ns = 0;
  if (argc >= 3 && JS_IsObject(argv[2]))
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[2], "maxMismatches");
    if (!JS_IsUndefined(v))
    {
      JS_ToUint32(ctx, &max_mismatches, v);
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[2], "fieldToleranceNs");
    if (!JS_IsUndefined(v))
    {
      JS_ToInt64(ctx, &tolerance_ns, v);
    }
    JS_FreeValue(ctx, v);
  }

  FloxTapeDiffHandle h =
      flox_tape_diff_create(left, right, max_mismatches, tolerance_ns);
  std::string leftStr = left;
  std::string rightStr = right;
  JS_FreeCString(ctx, left);
  JS_FreeCString(ctx, right);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "tapeDiff: failed to read tape directory(ies)");
  }

  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "leftPath", JS_NewString(ctx, leftStr.c_str()));
  JS_SetPropertyStr(ctx, out, "rightPath", JS_NewString(ctx, rightStr.c_str()));
  JS_SetPropertyStr(ctx, out, "leftCount",
                    JS_NewInt64(ctx, static_cast<int64_t>(flox_tape_diff_left_count(h))));
  JS_SetPropertyStr(ctx, out, "rightCount",
                    JS_NewInt64(ctx, static_cast<int64_t>(flox_tape_diff_right_count(h))));

  uint64_t div_idx = 0;
  if (flox_tape_diff_first_divergence(h, &div_idx))
  {
    JS_SetPropertyStr(ctx, out, "firstDivergenceIndex",
                      JS_NewInt64(ctx, static_cast<int64_t>(div_idx)));
  }
  else
  {
    JS_SetPropertyStr(ctx, out, "firstDivergenceIndex", JS_NULL);
  }
  JS_SetPropertyStr(ctx, out, "equal",
                    JS_NewBool(ctx, flox_tape_diff_equal(h) != 0));

  const uint64_t mcount = flox_tape_diff_mismatch_count(h);
  JSValue mlist = JS_NewArray(ctx);
  if (mcount > 0)
  {
    std::vector<FloxTapeDiffMismatch> buf(mcount);
    flox_tape_diff_copy_mismatches(h, buf.data(), mcount);
    for (uint64_t i = 0; i < mcount; ++i)
    {
      JSValue entry = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, entry, "index",
                        JS_NewInt64(ctx, static_cast<int64_t>(buf[i].index)));
      auto putSide = [&](const char* key, const FloxTapeDiffTrade& t)
      {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "exchangeTsNs", JS_NewInt64(ctx, t.exchange_ts_ns));
        JS_SetPropertyStr(ctx, o, "symbolId", JS_NewUint32(ctx, t.symbol_id));
        JS_SetPropertyStr(ctx, o, "priceRaw", JS_NewInt64(ctx, t.price_raw));
        JS_SetPropertyStr(ctx, o, "qtyRaw", JS_NewInt64(ctx, t.qty_raw));
        JS_SetPropertyStr(ctx, o, "side", JS_NewUint32(ctx, t.side));
        JS_SetPropertyStr(ctx, entry, key, o);
      };
      putSide("left", buf[i].left);
      putSide("right", buf[i].right);
      JS_SetPropertyUint32(ctx, mlist, static_cast<uint32_t>(i), entry);
    }
  }
  JS_SetPropertyStr(ctx, out, "mismatches", mlist);

  flox_tape_diff_destroy(h);
  return out;
}

// ============================================================
// Execution algorithm bindings (TWAP / VWAP / Iceberg / POV)
// ============================================================

static JSValue js_exec_twap_create(JSContext* ctx, JSValueConst, int argc,
                                   JSValueConst* argv)
{
  if (argc < 8)
  {
    return JS_ThrowTypeError(ctx, "twap_create: need 8 args");
  }
  double target_qty;
  JS_ToFloat64(ctx, &target_qty, argv[0]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  uint32_t symbol = toUint32(ctx, argv[2]);
  uint8_t type = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  double limit_price;
  JS_ToFloat64(ctx, &limit_price, argv[4]);
  int64_t duration_ns = toInt64(ctx, argv[5]);
  uint32_t slice_count = toUint32(ctx, argv[6]);
  int64_t start_time_ns = toInt64(ctx, argv[7]);
  auto h = flox_exec_twap_create(target_qty, side, symbol, type, limit_price,
                                 duration_ns, slice_count, start_time_ns);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "twap_create: invalid args");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_exec_vwap_create(JSContext* ctx, JSValueConst, int argc,
                                   JSValueConst* argv)
{
  if (argc < 6)
  {
    return JS_ThrowTypeError(ctx, "vwap_create: need 6 args");
  }
  double target_qty;
  JS_ToFloat64(ctx, &target_qty, argv[0]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  uint32_t symbol = toUint32(ctx, argv[2]);
  uint8_t type = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  double limit_price;
  JS_ToFloat64(ctx, &limit_price, argv[4]);
  std::vector<int64_t> ts;
  std::vector<double> vol;
  JSValue lenVal = JS_GetPropertyStr(ctx, argv[5], "length");
  uint32_t len = 0;
  JS_ToUint32(ctx, &len, lenVal);
  JS_FreeValue(ctx, lenVal);
  for (uint32_t i = 0; i < len; ++i)
  {
    JSValue row = JS_GetPropertyUint32(ctx, argv[5], i);
    JSValue tsv = JS_GetPropertyUint32(ctx, row, 0);
    JSValue volv = JS_GetPropertyUint32(ctx, row, 1);
    ts.push_back(toInt64(ctx, tsv));
    double v;
    JS_ToFloat64(ctx, &v, volv);
    vol.push_back(v);
    JS_FreeValue(ctx, tsv);
    JS_FreeValue(ctx, volv);
    JS_FreeValue(ctx, row);
  }
  auto h = flox_exec_vwap_create(target_qty, side, symbol, type, limit_price,
                                 ts.empty() ? nullptr : ts.data(),
                                 vol.empty() ? nullptr : vol.data(),
                                 ts.size());
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "vwap_create: invalid args");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_exec_iceberg_create(JSContext* ctx, JSValueConst, int argc,
                                      JSValueConst* argv)
{
  if (argc < 6)
  {
    return JS_ThrowTypeError(ctx, "iceberg_create: need 6 args");
  }
  double target_qty;
  JS_ToFloat64(ctx, &target_qty, argv[0]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  uint32_t symbol = toUint32(ctx, argv[2]);
  uint8_t type = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  double limit_price, visible_qty;
  JS_ToFloat64(ctx, &limit_price, argv[4]);
  JS_ToFloat64(ctx, &visible_qty, argv[5]);
  auto h = flox_exec_iceberg_create(target_qty, side, symbol, type, limit_price,
                                    visible_qty);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "iceberg_create: invalid args");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_exec_pov_create(JSContext* ctx, JSValueConst, int argc,
                                  JSValueConst* argv)
{
  if (argc < 7)
  {
    return JS_ThrowTypeError(ctx, "pov_create: need 7 args");
  }
  double target_qty;
  JS_ToFloat64(ctx, &target_qty, argv[0]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  uint32_t symbol = toUint32(ctx, argv[2]);
  uint8_t type = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  double limit_price, rate, min_slice;
  JS_ToFloat64(ctx, &limit_price, argv[4]);
  JS_ToFloat64(ctx, &rate, argv[5]);
  JS_ToFloat64(ctx, &min_slice, argv[6]);
  auto h = flox_exec_pov_create(target_qty, side, symbol, type, limit_price, rate,
                                min_slice);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "pov_create: invalid args");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_exec_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_exec_destroy(static_cast<FloxExecAlgoHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_exec_step(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<FloxExecAlgoHandle>(getHandle(ctx, argv[0]));
  flox_exec_step(h, toInt64(ctx, argv[1]));
  size_t n = flox_exec_pending_count(h);
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < n; ++i)
  {
    FloxExecChildOrder c{};
    if (flox_exec_pending_at(h, i, &c))
    {
      JSValue o = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, o, "orderId", JS_NewInt64(ctx, static_cast<int64_t>(c.order_id)));
      JS_SetPropertyStr(ctx, o, "timestampNs", JS_NewInt64(ctx, c.timestamp_ns));
      JS_SetPropertyStr(ctx, o, "qty", JS_NewFloat64(ctx, c.qty));
      JS_SetPropertyStr(ctx, o, "price", JS_NewFloat64(ctx, c.price));
      JS_SetPropertyStr(ctx, o, "type",
                        JS_NewString(ctx, c.type == 1 ? "limit" : "market"));
      JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), o);
    }
  }
  flox_exec_clear_pending(h);
  return arr;
}

static JSValue js_exec_report_fill(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double q;
  JS_ToFloat64(ctx, &q, argv[1]);
  flox_exec_report_fill(static_cast<FloxExecAlgoHandle>(getHandle(ctx, argv[0])), q);
  return JS_UNDEFINED;
}

static JSValue js_exec_observe_volume(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double q;
  JS_ToFloat64(ctx, &q, argv[1]);
  flox_exec_observe_volume(static_cast<FloxExecAlgoHandle>(getHandle(ctx, argv[0])), q);
  return JS_UNDEFINED;
}

static JSValue js_exec_submitted_qty(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, flox_exec_submitted_qty(static_cast<FloxExecAlgoHandle>(getHandle(ctx, argv[0]))));
}

static JSValue js_exec_filled_qty(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, flox_exec_filled_qty(static_cast<FloxExecAlgoHandle>(getHandle(ctx, argv[0]))));
}

static JSValue js_exec_remaining_qty(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, flox_exec_remaining_qty(static_cast<FloxExecAlgoHandle>(getHandle(ctx, argv[0]))));
}

static JSValue js_exec_is_done(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewBool(
      ctx, flox_exec_is_done(static_cast<FloxExecAlgoHandle>(getHandle(ctx, argv[0]))) != 0);
}

// ============================================================
// Portfolio risk aggregator bindings
// ============================================================

static JSValue js_portfolio_risk_create(JSContext* ctx, JSValueConst, int argc,
                                        JSValueConst* argv)
{
  FloxPortfolioRiskRules rules{};
  double initial_equity = 0.0;
  auto readOpt = [&](JSValueConst obj, const char* key, uint8_t& has, double& v)
  {
    JSValue val = JS_GetPropertyStr(ctx, obj, key);
    if (!JS_IsUndefined(val) && !JS_IsNull(val))
    {
      has = 1;
      JS_ToFloat64(ctx, &v, val);
    }
    JS_FreeValue(ctx, val);
  };
  if (argc > 0 && JS_IsObject(argv[0]))
  {
    readOpt(argv[0], "maxDrawdownPct", rules.has_max_drawdown_pct, rules.max_drawdown_pct);
    readOpt(argv[0], "maxDailyLoss", rules.has_max_daily_loss, rules.max_daily_loss);
    readOpt(argv[0], "maxGrossExposure", rules.has_max_gross_exposure, rules.max_gross_exposure);
    readOpt(argv[0], "maxConcentrationPct", rules.has_max_concentration_pct, rules.max_concentration_pct);
  }
  if (argc > 1)
  {
    JS_ToFloat64(ctx, &initial_equity, argv[1]);
  }
  FloxPortfolioRiskHandle h = flox_portfolio_risk_create(&rules, initial_equity);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "PortfolioRiskAggregator: construction failed");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_portfolio_risk_destroy(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  flox_portfolio_risk_destroy(static_cast<FloxPortfolioRiskHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_portfolio_risk_update(JSContext* ctx, JSValueConst, int argc,
                                        JSValueConst* argv)
{
  if (argc < 3)
  {
    return JS_ThrowTypeError(ctx, "update(handle, name, fields)");
  }
  auto h = static_cast<FloxPortfolioRiskHandle>(getHandle(ctx, argv[0]));
  const char* name = JS_ToCString(ctx, argv[1]);
  if (!name)
  {
    return JS_ThrowTypeError(ctx, "update: name must be a string");
  }
  FloxStrategyAccountFields f{};
  uint8_t mask = 0;
  auto putDouble = [&](const char* key, uint8_t bit, double& out)
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[2], key);
    if (!JS_IsUndefined(v))
    {
      JS_ToFloat64(ctx, &out, v);
      mask |= bit;
    }
    JS_FreeValue(ctx, v);
  };
  auto putInt = [&](const char* key, uint8_t bit, uint64_t& out)
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[2], key);
    if (!JS_IsUndefined(v))
    {
      int64_t tmp = 0;
      JS_ToInt64(ctx, &tmp, v);
      out = static_cast<uint64_t>(tmp);
      mask |= bit;
    }
    JS_FreeValue(ctx, v);
  };
  putDouble("realizedPnl", 1u << 0, f.realized_pnl);
  putDouble("unrealizedPnl", 1u << 1, f.unrealized_pnl);
  putDouble("fees", 1u << 2, f.fees);
  putDouble("grossExposure", 1u << 3, f.gross_exposure);
  putDouble("netExposure", 1u << 4, f.net_exposure);
  putInt("tradeCount", 1u << 5, f.trade_count);
  flox_portfolio_risk_update(h, name, &f, mask);
  JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

static JSValue js_portfolio_risk_remove(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  auto h = static_cast<FloxPortfolioRiskHandle>(getHandle(ctx, argv[0]));
  const char* name = JS_ToCString(ctx, argv[1]);
  if (name)
  {
    flox_portfolio_risk_remove(h, name);
    JS_FreeCString(ctx, name);
  }
  return JS_UNDEFINED;
}

static JSValue js_portfolio_risk_reset_kill_switch(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  flox_portfolio_risk_reset_kill_switch(
      static_cast<FloxPortfolioRiskHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue breachToJsObject(JSContext* ctx, const FloxBreach& b)
{
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "rule", JS_NewString(ctx, b.rule ? b.rule : ""));
  JS_SetPropertyStr(ctx, o, "value", JS_NewFloat64(ctx, b.value));
  JS_SetPropertyStr(ctx, o, "limit", JS_NewFloat64(ctx, b.limit));
  JS_SetPropertyStr(ctx, o, "detail", JS_NewString(ctx, b.detail ? b.detail : ""));
  return o;
}

static JSValue js_portfolio_risk_check_order(JSContext* ctx, JSValueConst, int argc,
                                             JSValueConst* argv)
{
  if (argc < 4)
  {
    return JS_ThrowTypeError(ctx, "checkOrder(handle, strategy, notional, side)");
  }
  auto h = static_cast<FloxPortfolioRiskHandle>(getHandle(ctx, argv[0]));
  const char* strat = JS_ToCString(ctx, argv[1]);
  double notional = 0.0;
  JS_ToFloat64(ctx, &notional, argv[2]);
  const char* side = JS_ToCString(ctx, argv[3]);
  FloxBreach b{};
  uint8_t hit = flox_portfolio_risk_check_order(h, strat ? strat : "",
                                                notional, side ? side : "", &b);
  JSValue out = hit ? breachToJsObject(ctx, b) : JS_NULL;
  if (strat)
  {
    JS_FreeCString(ctx, strat);
  }
  if (side)
  {
    JS_FreeCString(ctx, side);
  }
  return out;
}

static JSValue js_portfolio_risk_snapshot(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  auto h = static_cast<FloxPortfolioRiskHandle>(getHandle(ctx, argv[0]));
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "totalDailyPnl",
                    JS_NewFloat64(ctx, flox_portfolio_risk_total_daily_pnl(h)));
  JS_SetPropertyStr(ctx, o, "totalGrossExposure",
                    JS_NewFloat64(ctx, flox_portfolio_risk_total_gross_exposure(h)));
  JS_SetPropertyStr(ctx, o, "currentEquity",
                    JS_NewFloat64(ctx, flox_portfolio_risk_current_equity(h)));
  JS_SetPropertyStr(ctx, o, "drawdownPct",
                    JS_NewFloat64(ctx, flox_portfolio_risk_drawdown_pct(h)));
  JS_SetPropertyStr(ctx, o, "killSwitchActive",
                    JS_NewBool(ctx, flox_portfolio_risk_kill_switch_active(h) != 0));
  const uint64_t bn = flox_portfolio_risk_breach_count(h);
  JSValue arr = JS_NewArray(ctx);
  for (uint64_t i = 0; i < bn; ++i)
  {
    FloxBreach b{};
    if (flox_portfolio_risk_breach_at(h, i, &b))
    {
      JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), breachToJsObject(ctx, b));
    }
  }
  JS_SetPropertyStr(ctx, o, "breaches", arr);
  JS_SetPropertyStr(ctx, o, "accountCount",
                    JS_NewInt64(ctx, static_cast<int64_t>(flox_portfolio_risk_account_count(h))));
  return o;
}

// ============================================================
// Latency model bindings
// ============================================================

static JSValue js_lat_constant_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  int64_t feed = (argc > 0) ? toInt64(ctx, argv[0]) : 0;
  int64_t order = (argc > 1) ? toInt64(ctx, argv[1]) : 0;
  int64_t fill = (argc > 2) ? toInt64(ctx, argv[2]) : 0;
  FloxLatencyModelHandle h = flox_latency_constant_create(feed, order, fill);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "ConstantLatency: feed/order/fill must be non-negative");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_lat_gaussian_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  double fm = (argc > 0) ? toDouble(ctx, argv[0]) : 0.0;
  double fs = (argc > 1) ? toDouble(ctx, argv[1]) : 0.0;
  double om = (argc > 2) ? toDouble(ctx, argv[2]) : 0.0;
  double os = (argc > 3) ? toDouble(ctx, argv[3]) : 0.0;
  double lm = (argc > 4) ? toDouble(ctx, argv[4]) : 0.0;
  double ls = (argc > 5) ? toDouble(ctx, argv[5]) : 0.0;
  uint64_t seed = (argc > 6) ? static_cast<uint64_t>(toInt64(ctx, argv[6])) : 0;
  FloxLatencyModelHandle h = flox_latency_gaussian_create(fm, fs, om, os, lm, ls, seed);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "GaussianLatency: means and stddevs must be non-negative");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_lat_exponential_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  double fm = (argc > 0) ? toDouble(ctx, argv[0]) : 0.0;
  double om = (argc > 1) ? toDouble(ctx, argv[1]) : 0.0;
  double lm = (argc > 2) ? toDouble(ctx, argv[2]) : 0.0;
  uint64_t seed = (argc > 3) ? static_cast<uint64_t>(toInt64(ctx, argv[3])) : 0;
  FloxLatencyModelHandle h = flox_latency_exponential_create(fm, om, lm, seed);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "ExponentialLatency: means must be non-negative");
  }
  return createHandleObject(ctx, h);
}

static std::vector<int64_t> readJsInt64Array(JSContext* ctx, JSValueConst arr)
{
  std::vector<int64_t> out;
  if (!JS_IsArray(ctx, arr))
  {
    return out;
  }
  uint32_t len = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
  JS_ToUint32(ctx, &len, lenVal);
  JS_FreeValue(ctx, lenVal);
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, arr, i);
    out.push_back(toInt64(ctx, v));
    JS_FreeValue(ctx, v);
  }
  return out;
}

static JSValue js_lat_empirical_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  std::vector<int64_t> feed = (argc > 0) ? readJsInt64Array(ctx, argv[0]) : std::vector<int64_t>{};
  std::vector<int64_t> order = (argc > 1) ? readJsInt64Array(ctx, argv[1]) : std::vector<int64_t>{};
  std::vector<int64_t> fill = (argc > 2) ? readJsInt64Array(ctx, argv[2]) : std::vector<int64_t>{};
  uint64_t seed = (argc > 3) ? static_cast<uint64_t>(toInt64(ctx, argv[3])) : 0;
  FloxLatencyModelHandle h = flox_latency_empirical_create(
      feed.empty() ? nullptr : feed.data(), feed.size(),
      order.empty() ? nullptr : order.data(), order.size(),
      fill.empty() ? nullptr : fill.data(), fill.size(),
      seed);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "EmpiricalLatency: provide non-empty samples and non-negative values");
  }
  return createHandleObject(ctx, h);
}

static JSValue js_lat_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  flox_latency_destroy(static_cast<FloxLatencyModelHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_lat_feed_delay(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewInt64(ctx, flox_latency_feed_delay(
                              static_cast<FloxLatencyModelHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_lat_order_delay(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewInt64(ctx, flox_latency_order_delay(
                              static_cast<FloxLatencyModelHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_lat_fill_delay(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewInt64(ctx, flox_latency_fill_delay(
                              static_cast<FloxLatencyModelHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_lat_sample(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  FloxLatencySample s{};
  flox_latency_sample(static_cast<FloxLatencyModelHandle>(getHandle(ctx, argv[0])), &s);
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "feedNs", JS_NewInt64(ctx, s.feed_ns));
  JS_SetPropertyStr(ctx, obj, "orderNs", JS_NewInt64(ctx, s.order_ns));
  JS_SetPropertyStr(ctx, obj, "fillNs", JS_NewInt64(ctx, s.fill_ns));
  return obj;
}
static JSValue js_lat_reset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  uint64_t seed = (argc > 1) ? static_cast<uint64_t>(toInt64(ctx, argv[1])) : 0;
  flox_latency_reset(static_cast<FloxLatencyModelHandle>(getHandle(ctx, argv[0])), seed);
  return JS_UNDEFINED;
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
// OrderJourneyTracer
// ============================================================

static JSValue js_ojt_create(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  uint64_t maxOrders = (argc > 0) ? static_cast<uint64_t>(toInt64(c, a[0])) : 1000000;
  uint64_t maxRecords = (argc > 1) ? static_cast<uint64_t>(toInt64(c, a[1])) : 64;
  double sampleRate = (argc > 2) ? toDouble(c, a[2]) : 1.0;
  uint64_t sampleSalt =
      (argc > 3) ? static_cast<uint64_t>(toInt64(c, a[3])) : 0x9E3779B97F4A7C15ULL;
  return createHandleObject(c, flox_order_journey_tracer_create(
                                   maxOrders, maxRecords, sampleRate, sampleSalt));
}

static JSValue js_ojt_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_order_journey_tracer_destroy(
      static_cast<FloxOrderJourneyTracerHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_ojt_order_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewInt64(c, static_cast<int64_t>(flox_order_journey_tracer_order_count(
                            static_cast<FloxOrderJourneyTracerHandle>(getHandle(c, a[0])))));
}

static JSValue js_ojt_record_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewInt64(c, static_cast<int64_t>(flox_order_journey_tracer_record_count(
                            static_cast<FloxOrderJourneyTracerHandle>(getHandle(c, a[0])))));
}

static JSValue js_ojt_median_ack(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(
      c, flox_order_journey_tracer_median_ack_latency_ns(
             static_cast<FloxOrderJourneyTracerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ojt_median_ttf(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(
      c, flox_order_journey_tracer_median_time_to_first_fill_ns(
             static_cast<FloxOrderJourneyTracerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ojt_maker_ratio(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, flox_order_journey_tracer_maker_fill_ratio(
                              static_cast<FloxOrderJourneyTracerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ojt_cancel_race(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, flox_order_journey_tracer_cancel_race_loss_rate(
                              static_cast<FloxOrderJourneyTracerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ojt_row_to_obj(JSContext* c, const FloxOrderTraceRow& r)
{
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "orderId", JS_NewInt64(c, static_cast<int64_t>(r.order_id)));
  JS_SetPropertyStr(c, o, "seq", JS_NewUint32(c, r.seq));
  JS_SetPropertyStr(c, o, "status", JS_NewUint32(c, r.status));
  JS_SetPropertyStr(c, o, "isMaker", JS_NewBool(c, r.is_maker != 0));
  JS_SetPropertyStr(c, o, "tsNs", JS_NewInt64(c, r.ts_ns));
  JS_SetPropertyStr(c, o, "fillQty",
                    JS_NewFloat64(c, static_cast<double>(r.fill_qty_raw) / 1e8));
  JS_SetPropertyStr(c, o, "fillPrice",
                    JS_NewFloat64(c, static_cast<double>(r.fill_price_raw) / 1e8));
  JS_SetPropertyStr(c, o, "queueAhead",
                    JS_NewFloat64(c, static_cast<double>(r.queue_ahead_raw) / 1e8));
  JS_SetPropertyStr(c, o, "queueTotal",
                    JS_NewFloat64(c, static_cast<double>(r.queue_total_raw) / 1e8));
  JS_SetPropertyStr(c, o, "submittedAtNs", JS_NewInt64(c, r.submitted_at_ns));
  JS_SetPropertyStr(c, o, "acceptedAtNs", JS_NewInt64(c, r.accepted_at_ns));
  JS_SetPropertyStr(c, o, "firstFillAtNs", JS_NewInt64(c, r.first_fill_at_ns));
  JS_SetPropertyStr(c, o, "lastFillAtNs", JS_NewInt64(c, r.last_fill_at_ns));
  JS_SetPropertyStr(c, o, "canceledAtNs", JS_NewInt64(c, r.canceled_at_ns));
  JS_SetPropertyStr(c, o, "rejectedAtNs", JS_NewInt64(c, r.rejected_at_ns));
  JS_SetPropertyStr(c, o, "triggeredAtNs", JS_NewInt64(c, r.triggered_at_ns));
  JS_SetPropertyStr(c, o, "expiredAtNs", JS_NewInt64(c, r.expired_at_ns));
  return o;
}

static JSValue js_ojt_result(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxOrderJourneyTracerHandle>(getHandle(c, a[0]));
  const uint64_t n = flox_order_journey_tracer_result(h, nullptr, 0);
  std::vector<FloxOrderTraceRow> buf(n);
  if (n > 0)
  {
    flox_order_journey_tracer_result(h, buf.data(), n);
  }
  JSValue arr = JS_NewArray(c);
  for (uint64_t i = 0; i < n; ++i)
  {
    JS_SetPropertyUint32(c, arr, static_cast<uint32_t>(i), js_ojt_row_to_obj(c, buf[i]));
  }
  return arr;
}

static JSValue js_ojt_journey(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxOrderJourneyTracerHandle>(getHandle(c, a[0]));
  uint64_t orderId = static_cast<uint64_t>(toInt64(c, a[1]));
  const uint64_t n = flox_order_journey_tracer_journey(h, orderId, nullptr, 0);
  std::vector<FloxOrderTraceRow> buf(n);
  if (n > 0)
  {
    flox_order_journey_tracer_journey(h, orderId, buf.data(), n);
  }
  JSValue arr = JS_NewArray(c);
  for (uint64_t i = 0; i < n; ++i)
  {
    JS_SetPropertyUint32(c, arr, static_cast<uint32_t>(i), js_ojt_row_to_obj(c, buf[i]));
  }
  return arr;
}

static JSValue js_ojt_clear(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_order_journey_tracer_clear(
      static_cast<FloxOrderJourneyTracerHandle>(getHandle(c, a[0])));
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

// Extract raw int64 [price,qty,...] pairs from a BigInt64Array (or ArrayBuffer) into a
// vector<FloxBookLevel>. Returns true on success; throws on type/size error.
static bool extractBookLevels(JSContext* c, JSValueConst arr, uint32_t expectedPairs,
                              std::vector<FloxBookLevel>& out)
{
  if (expectedPairs == 0)
  {
    out.clear();
    return true;
  }
  if (JS_IsUndefined(arr) || JS_IsNull(arr))
  {
    JS_ThrowTypeError(c, "writeBook: levels buffer is null but count > 0");
    return false;
  }
  size_t byteOffset = 0;
  size_t byteLength = 0;
  size_t bytesPerElement = 0;
  JSValue ab = JS_GetTypedArrayBuffer(c, arr, &byteOffset, &byteLength, &bytesPerElement);
  uint8_t* base = nullptr;
  size_t totalSize = 0;
  if (!JS_IsException(ab))
  {
    base = JS_GetArrayBuffer(c, &totalSize, ab);
    JS_FreeValue(c, ab);
  }
  else
  {
    // Maybe a plain ArrayBuffer.
    JS_GetException(c);  // clear pending
    base = JS_GetArrayBuffer(c, &totalSize, arr);
    byteOffset = 0;
    byteLength = totalSize;
    bytesPerElement = 8;
  }
  if (!base)
  {
    JS_ThrowTypeError(c, "writeBook: expected BigInt64Array or ArrayBuffer");
    return false;
  }
  if (bytesPerElement != 0 && bytesPerElement != 8)
  {
    JS_ThrowTypeError(c, "writeBook: typed array must be BigInt64Array (8-byte elements)");
    return false;
  }
  size_t needed = static_cast<size_t>(expectedPairs) * 2 * 8;
  if (byteLength < needed)
  {
    JS_ThrowRangeError(c, "writeBook: buffer too small for declared level count");
    return false;
  }
  const int64_t* p = reinterpret_cast<const int64_t*>(base + byteOffset);
  out.resize(expectedPairs);
  for (uint32_t i = 0; i < expectedPairs; i++)
  {
    out[i].price_raw = p[i * 2];
    out[i].quantity_raw = p[i * 2 + 1];
  }
  return true;
}

static JSValue js_dw_write_book(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxDataWriterHandle>(getHandle(c, a[0]));
  int64_t exchangeTsNs = toInt64(c, a[1]);
  int64_t recvTsNs = toInt64(c, a[2]);
  int64_t seq = toInt64(c, a[3]);
  uint32_t symbolId = toUint32(c, a[4]);
  uint8_t isSnapshot = static_cast<uint8_t>(toUint32(c, a[5]));
  uint32_t nBids = toUint32(c, a[7]);
  uint32_t nAsks = toUint32(c, a[9]);

  std::vector<FloxBookLevel> bids;
  std::vector<FloxBookLevel> asks;
  if (!extractBookLevels(c, a[6], nBids, bids))
  {
    return JS_EXCEPTION;
  }
  if (!extractBookLevels(c, a[8], nAsks, asks))
  {
    return JS_EXCEPTION;
  }
  uint8_t ok = flox_data_writer_write_book(h, exchangeTsNs, recvTsNs, seq, symbolId, isSnapshot,
                                           bids.empty() ? nullptr : bids.data(), nBids,
                                           asks.empty() ? nullptr : asks.data(), nAsks);
  return JS_NewBool(c, ok);
}

// ============================================================
// BinaryLogRecorderHook
// ============================================================

static JSValue js_blrh_create(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  const char* dir = JS_ToCString(c, a[0]);
  uint64_t mb = (argc < 2 || JS_IsUndefined(a[1]) || JS_IsNull(a[1]))
                    ? 256
                    : static_cast<uint64_t>(toInt64(c, a[1]));
  uint8_t eid = (argc < 3 || JS_IsUndefined(a[2]) || JS_IsNull(a[2]))
                    ? 0
                    : static_cast<uint8_t>(toUint32(c, a[2]));
  uint8_t comp = (argc < 4 || JS_IsUndefined(a[3]) || JS_IsNull(a[3]))
                     ? 0
                     : static_cast<uint8_t>(toUint32(c, a[3]));
  // Optional a[4] = exchange_name, a[5] = instrument_type. Both feed the
  // RecordingMetadata stamp the merged-tape reader keys on.
  const char* exch = (argc > 4 && JS_IsString(a[4])) ? JS_ToCString(c, a[4]) : nullptr;
  const char* itype = (argc > 5 && JS_IsString(a[5])) ? JS_ToCString(c, a[5]) : nullptr;
  JSValue ret = createHandleObject(
      c, flox_binary_log_recorder_hook_create_ex(dir, mb, eid, comp, exch, itype));
  JS_FreeCString(c, dir);
  if (exch)
  {
    JS_FreeCString(c, exch);
  }
  if (itype)
  {
    JS_FreeCString(c, itype);
  }
  return ret;
}

// ── Recorder-handle drivers (used by smoke tests + bindings that want
//    to push events directly through a BinaryLogRecorderHook without
//    spinning up a Runner). The recorder handle's first member is the
//    callbacks struct, so the cast is layout-stable.
static JSValue js_recorder_on_start(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto* cb = static_cast<FloxMarketDataRecorderCallbacks*>(getHandle(c, a[0]));
  if (cb && cb->on_start)
  {
    cb->on_start(cb->user_data);
  }
  return JS_UNDEFINED;
}

static JSValue js_recorder_on_stop(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto* cb = static_cast<FloxMarketDataRecorderCallbacks*>(getHandle(c, a[0]));
  if (cb && cb->on_stop)
  {
    cb->on_stop(cb->user_data);
  }
  return JS_UNDEFINED;
}

static JSValue js_recorder_on_trade(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  // (handle, symbolId, price, qty, isBuy, exchangeTsNs)
  auto* cb = static_cast<FloxMarketDataRecorderCallbacks*>(getHandle(c, a[0]));
  if (!cb || !cb->on_trade)
  {
    return JS_UNDEFINED;
  }
  FloxTradeData td{};
  td.symbol = toUint32(c, a[1]);
  double px = toDouble(c, a[2]);
  double qty = toDouble(c, a[3]);
  td.price_raw = static_cast<int64_t>(px * 1e8);
  td.quantity_raw = static_cast<int64_t>(qty * 1e8);
  td.is_buy = JS_ToBool(c, a[4]) ? 1 : 0;
  td.exchange_ts_ns = toInt64(c, a[5]);
  cb->on_trade(cb->user_data, &td);
  return JS_UNDEFINED;
}

static JSValue js_blrh_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_binary_log_recorder_hook_destroy(
      static_cast<FloxBinaryLogRecorderHookHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_blrh_as_recorder(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto rec = flox_binary_log_recorder_hook_as_recorder(
      static_cast<FloxBinaryLogRecorderHookHandle>(getHandle(c, a[0])));
  return createHandleObject(c, rec);
}

static JSValue js_blrh_add_symbol(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<FloxBinaryLogRecorderHookHandle>(getHandle(c, a[0]));
  uint32_t symbolId = toUint32(c, a[1]);
  const char* name = JS_ToCString(c, a[2]);
  const char* base = argc > 3 ? JS_ToCString(c, a[3]) : nullptr;
  const char* quote = argc > 4 ? JS_ToCString(c, a[4]) : nullptr;
  int8_t pp = argc > 5 ? static_cast<int8_t>(toInt64(c, a[5])) : 8;
  int8_t qp = argc > 6 ? static_cast<int8_t>(toInt64(c, a[6])) : 8;
  flox_binary_log_recorder_hook_add_symbol(h, symbolId, name ? name : "", base ? base : "",
                                           quote ? quote : "", pp, qp);
  if (name)
  {
    JS_FreeCString(c, name);
  }
  if (base)
  {
    JS_FreeCString(c, base);
  }
  if (quote)
  {
    JS_FreeCString(c, quote);
  }
  return JS_UNDEFINED;
}

static JSValue js_blrh_flush(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_binary_log_recorder_hook_flush(
      static_cast<FloxBinaryLogRecorderHookHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_blrh_stats(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  FloxWriterStats s = flox_binary_log_recorder_hook_stats(
      static_cast<FloxBinaryLogRecorderHookHandle>(getHandle(c, a[0])));
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "tradesWritten", JS_NewInt64(c, static_cast<int64_t>(s.trades_written)));
  JS_SetPropertyStr(c, o, "bookUpdatesWritten",
                    JS_NewInt64(c, static_cast<int64_t>(s.events_written - s.trades_written)));
  JS_SetPropertyStr(c, o, "bytesWritten", JS_NewInt64(c, static_cast<int64_t>(s.bytes_written)));
  JS_SetPropertyStr(c, o, "segmentsCreated",
                    JS_NewInt64(c, static_cast<int64_t>(s.segments_created)));
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

// ── helpers shared by read_X / read_X_from ───────────────────────────────

static JSValue js_dr_build_trades_array(JSContext* c, const std::vector<FloxTradeRecord>& trades,
                                        uint64_t got)
{
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

static JSValue js_dr_build_bbos_array(JSContext* c, const std::vector<FloxBBO>& bbos, uint64_t got)
{
  JSValue arr = JS_NewArray(c);
  for (uint64_t i = 0; i < got; i++)
  {
    const auto& b = bbos[i];
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "exchangeTsNs", JS_NewInt64(c, b.exchange_ts_ns));
    JS_SetPropertyStr(c, o, "recvTsNs", JS_NewInt64(c, b.recv_ts_ns));
    JS_SetPropertyStr(c, o, "seq", JS_NewInt64(c, b.seq));
    JS_SetPropertyStr(c, o, "symbolId", JS_NewUint32(c, b.symbol_id));
    JS_SetPropertyStr(c, o, "eventType", JS_NewUint32(c, b.event_type));
    JS_SetPropertyStr(c, o, "bidPrice",
                      JS_NewFloat64(c, static_cast<double>(b.bid_price_raw) / 1e8));
    JS_SetPropertyStr(c, o, "bidQty",
                      JS_NewFloat64(c, static_cast<double>(b.bid_qty_raw) / 1e8));
    JS_SetPropertyStr(c, o, "askPrice",
                      JS_NewFloat64(c, static_cast<double>(b.ask_price_raw) / 1e8));
    JS_SetPropertyStr(c, o, "askQty",
                      JS_NewFloat64(c, static_cast<double>(b.ask_qty_raw) / 1e8));
    JS_SetPropertyUint32(c, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
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
  return js_dr_build_trades_array(c, trades, got);
}

static JSValue js_dr_read_trades_from(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<FloxDataReaderHandle>(getHandle(c, a[0]));
  int64_t startTsNs = toInt64(c, a[1]);
  uint64_t max = argc > 2 ? static_cast<uint64_t>(toInt64(c, a[2])) : 0;
  uint64_t n = flox_data_reader_read_trades_from(h, startTsNs, nullptr, 0);
  if (max > 0 && max < n)
  {
    n = max;
  }
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxTradeRecord> trades(n);
  uint64_t got = flox_data_reader_read_trades_from(h, startTsNs, trades.data(), n);
  return js_dr_build_trades_array(c, trades, got);
}

static JSValue js_dr_read_bbo(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<FloxDataReaderHandle>(getHandle(c, a[0]));
  uint64_t max = argc > 1 ? static_cast<uint64_t>(toInt64(c, a[1])) : 0;
  uint64_t n = flox_data_reader_read_bbo(h, nullptr, 0);
  if (max > 0 && max < n)
  {
    n = max;
  }
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxBBO> bbos(n);
  uint64_t got = flox_data_reader_read_bbo(h, bbos.data(), n);
  return js_dr_build_bbos_array(c, bbos, got);
}

static JSValue js_dr_read_bbo_from(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<FloxDataReaderHandle>(getHandle(c, a[0]));
  int64_t startTsNs = toInt64(c, a[1]);
  uint64_t max = argc > 2 ? static_cast<uint64_t>(toInt64(c, a[2])) : 0;
  uint64_t n = flox_data_reader_read_bbo_from(h, startTsNs, nullptr, 0);
  if (max > 0 && max < n)
  {
    n = max;
  }
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxBBO> bbos(n);
  uint64_t got = flox_data_reader_read_bbo_from(h, startTsNs, bbos.data(), n);
  return js_dr_build_bbos_array(c, bbos, got);
}

static JSValue js_dr_build_book_updates_array(JSContext* c,
                                              const std::vector<FloxBookUpdateHeader>& headers,
                                              const std::vector<FloxLevel>& levels, uint64_t got)
{
  JSValue arr = JS_NewArray(c);
  for (uint64_t i = 0; i < got; i++)
  {
    const auto& hdr = headers[i];
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "exchangeTsNs", JS_NewInt64(c, hdr.exchange_ts_ns));
    JS_SetPropertyStr(c, o, "recvTsNs", JS_NewInt64(c, hdr.recv_ts_ns));
    JS_SetPropertyStr(c, o, "seq", JS_NewInt64(c, hdr.seq));
    JS_SetPropertyStr(c, o, "symbolId", JS_NewUint32(c, hdr.symbol_id));
    JS_SetPropertyStr(c, o, "eventType", JS_NewUint32(c, hdr.event_type));

    JSValue bids = JS_NewArray(c);
    for (uint16_t k = 0; k < hdr.bid_count; k++)
    {
      const auto& l = levels[hdr.level_offset + k];
      JSValue lo = JS_NewObject(c);
      JS_SetPropertyStr(c, lo, "price", JS_NewFloat64(c, static_cast<double>(l.price_raw) / 1e8));
      JS_SetPropertyStr(c, lo, "qty", JS_NewFloat64(c, static_cast<double>(l.qty_raw) / 1e8));
      JS_SetPropertyUint32(c, bids, k, lo);
    }
    JS_SetPropertyStr(c, o, "bids", bids);

    JSValue asks = JS_NewArray(c);
    for (uint16_t k = 0; k < hdr.ask_count; k++)
    {
      const auto& l = levels[hdr.level_offset + hdr.bid_count + k];
      JSValue lo = JS_NewObject(c);
      JS_SetPropertyStr(c, lo, "price", JS_NewFloat64(c, static_cast<double>(l.price_raw) / 1e8));
      JS_SetPropertyStr(c, lo, "qty", JS_NewFloat64(c, static_cast<double>(l.qty_raw) / 1e8));
      JS_SetPropertyUint32(c, asks, k, lo);
    }
    JS_SetPropertyStr(c, o, "asks", asks);

    JS_SetPropertyUint32(c, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

static JSValue js_dr_read_book_updates(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxDataReaderHandle>(getHandle(c, a[0]));
  uint64_t total_levels = 0;
  uint64_t n_events = flox_data_reader_count_book_updates(h, &total_levels);
  if (n_events == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxBookUpdateHeader> headers(n_events);
  std::vector<FloxLevel> levels(total_levels);
  uint64_t got = flox_data_reader_read_book_updates(h, headers.data(), n_events, levels.data(),
                                                    total_levels);
  return js_dr_build_book_updates_array(c, headers, levels, got);
}

static JSValue js_dr_read_book_updates_from(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxDataReaderHandle>(getHandle(c, a[0]));
  int64_t startTsNs = toInt64(c, a[1]);
  uint64_t total_levels = 0;
  uint64_t n_events = flox_data_reader_count_book_updates_from(h, startTsNs, &total_levels);
  if (n_events == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxBookUpdateHeader> headers(n_events);
  std::vector<FloxLevel> levels(total_levels);
  uint64_t got = flox_data_reader_read_book_updates_from(
      h, startTsNs, headers.data(), n_events, levels.data(), total_levels);
  return js_dr_build_book_updates_array(c, headers, levels, got);
}

// ============================================================
// MergedTapeReader — N-tape merged consumption
// ============================================================

static JSValue js_mtr_create(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  // a[0]: array of paths (string[])
  // a[1]: fromNs (int64, -1 = none)
  // a[2]: toNs   (int64, -1 = none)
  // a[3]: symbol filter (uint32[], optional)
  std::vector<const char*> path_cstrs;
  std::vector<JSValue> path_jsvals;  // hold for JS_FreeCString
  if (JS_IsArray(c, a[0]))
  {
    JSValue lenVal = JS_GetPropertyStr(c, a[0], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lenVal);
    JS_FreeValue(c, lenVal);
    path_cstrs.reserve(n);
    path_jsvals.reserve(n);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[0], i);
      const char* s = JS_ToCString(c, e);
      path_cstrs.push_back(s ? s : "");
      path_jsvals.push_back(e);  // freed after C call
    }
  }
  int64_t from_ns = argc > 1 ? toInt64(c, a[1]) : -1;
  int64_t to_ns = argc > 2 ? toInt64(c, a[2]) : -1;

  std::vector<uint32_t> syms;
  if (argc > 3 && JS_IsArray(c, a[3]))
  {
    JSValue lenVal = JS_GetPropertyStr(c, a[3], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lenVal);
    JS_FreeValue(c, lenVal);
    syms.reserve(n);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[3], i);
      syms.push_back(toUint32(c, e));
      JS_FreeValue(c, e);
    }
  }

  auto handle = flox_merged_tape_reader_create(
      path_cstrs.empty() ? nullptr : path_cstrs.data(),
      static_cast<uint32_t>(path_cstrs.size()), from_ns, to_ns,
      syms.empty() ? nullptr : syms.data(), static_cast<uint32_t>(syms.size()));

  // Release borrowed strings + JS values
  for (size_t i = 0; i < path_cstrs.size(); ++i)
  {
    if (path_cstrs[i])
    {
      JS_FreeCString(c, path_cstrs[i]);
    }
    JS_FreeValue(c, path_jsvals[i]);
  }

  return createHandleObject(c, handle);
}

static JSValue js_mtr_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  flox_merged_tape_reader_destroy(
      static_cast<FloxMergedTapeReaderHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_mtr_symbol_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewUint32(c, flox_merged_tape_reader_symbol_count(
                             static_cast<FloxMergedTapeReaderHandle>(getHandle(c, a[0]))));
}

static JSValue js_mtr_get_symbols(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxMergedTapeReaderHandle>(getHandle(c, a[0]));
  uint32_t n = flox_merged_tape_reader_symbol_count(h);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxMergedSymbol> syms(n);
  uint32_t got = flox_merged_tape_reader_get_symbols(h, syms.data(), n);
  JSValue arr = JS_NewArray(c);
  for (uint32_t i = 0; i < got; i++)
  {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "globalId", JS_NewUint32(c, syms[i].global_id));
    JS_SetPropertyStr(c, o, "pricePrecision",
                      JS_NewInt32(c, static_cast<int32_t>(syms[i].price_precision)));
    JS_SetPropertyStr(c, o, "qtyPrecision",
                      JS_NewInt32(c, static_cast<int32_t>(syms[i].qty_precision)));
    JS_SetPropertyStr(c, o, "exchange", JS_NewString(c, syms[i].exchange ? syms[i].exchange : ""));
    JS_SetPropertyStr(c, o, "name", JS_NewString(c, syms[i].name ? syms[i].name : ""));
    JS_SetPropertyUint32(c, arr, i, o);
  }
  return arr;
}

static JSValue js_mtr_tape_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewUint32(c, flox_merged_tape_reader_tape_count(
                             static_cast<FloxMergedTapeReaderHandle>(getHandle(c, a[0]))));
}

static JSValue js_mtr_get_tape_stats(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxMergedTapeReaderHandle>(getHandle(c, a[0]));
  uint32_t n = flox_merged_tape_reader_tape_count(h);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxMergedTapeStats> stats(n);
  uint32_t got = flox_merged_tape_reader_get_tape_stats(h, stats.data(), n);
  JSValue arr = JS_NewArray(c);
  for (uint32_t i = 0; i < got; i++)
  {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "firstEventNs", JS_NewBigInt64(c, stats[i].first_event_ns));
    JS_SetPropertyStr(c, o, "lastEventNs", JS_NewBigInt64(c, stats[i].last_event_ns));
    JS_SetPropertyStr(c, o, "trades",
                      JS_NewBigInt64(c, static_cast<int64_t>(stats[i].trades)));
    JS_SetPropertyStr(c, o, "books", JS_NewBigInt64(c, static_cast<int64_t>(stats[i].books)));
    JS_SetPropertyStr(c, o, "path", JS_NewString(c, stats[i].path ? stats[i].path : ""));
    JS_SetPropertyUint32(c, arr, i, o);
  }
  return arr;
}

static JSValue js_mtr_time_range(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxMergedTapeReaderHandle>(getHandle(c, a[0]));
  int64_t lo = 0, hi = 0;
  flox_merged_tape_reader_time_range(h, &lo, &hi);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "minFirstNs", JS_NewBigInt64(c, lo));
  JS_SetPropertyStr(c, o, "maxLastNs", JS_NewBigInt64(c, hi));
  return o;
}

static JSValue js_mtr_count_trades(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBigInt64(c, static_cast<int64_t>(flox_merged_tape_reader_count_trades(
                               static_cast<FloxMergedTapeReaderHandle>(getHandle(c, a[0])))));
}

static JSValue js_mtr_read_trades(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<FloxMergedTapeReaderHandle>(getHandle(c, a[0]));
  uint64_t max = argc > 1 ? static_cast<uint64_t>(toInt64(c, a[1])) : 0;
  uint64_t n = flox_merged_tape_reader_count_trades(h);
  if (max > 0 && max < n)
  {
    n = max;
  }
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxTradeRecord> trades(n);
  uint64_t got = flox_merged_tape_reader_read_trades(h, trades.data(), n);
  return js_dr_build_trades_array(c, trades, got);
}

static JSValue js_mtr_count_books(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxMergedTapeReaderHandle>(getHandle(c, a[0]));
  uint64_t total_levels = 0;
  uint64_t n_events = flox_merged_tape_reader_count_books(h, &total_levels);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "events", JS_NewBigInt64(c, static_cast<int64_t>(n_events)));
  JS_SetPropertyStr(c, o, "levels", JS_NewBigInt64(c, static_cast<int64_t>(total_levels)));
  return o;
}

static JSValue js_mtr_read_books(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<FloxMergedTapeReaderHandle>(getHandle(c, a[0]));
  uint64_t total_levels = 0;
  uint64_t n_events = flox_merged_tape_reader_count_books(h, &total_levels);
  if (n_events == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<FloxBookUpdateHeader> headers(n_events);
  std::vector<FloxLevel> levels(total_levels);
  uint64_t got = flox_merged_tape_reader_read_books(h, headers.data(), n_events,
                                                    levels.data(), total_levels);
  return js_dr_build_book_updates_array(c, headers, levels, got);
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
  addGlobalFunc(ctx, "__flox_strategy_last_closed_bar", js_strategy_last_closed_bar, 4);
  addGlobalFunc(ctx, "__flox_strategy_last_n_closed_bars", js_strategy_last_n_closed_bars, 5);
  addGlobalFunc(ctx, "__flox_strategy_get_bar_ring_capacity",
                js_strategy_get_bar_ring_capacity, 1);
  addGlobalFunc(ctx, "__flox_strategy_set_bar_ring_capacity",
                js_strategy_set_bar_ring_capacity, 2);
  addGlobalFunc(ctx, "__flox_feed_clock_create", js_feed_clock_create, 6);
  addGlobalFunc(ctx, "__flox_feed_clock_destroy", js_feed_clock_destroy, 1);
  addGlobalFunc(ctx, "__flox_feed_clock_symbol_count", js_feed_clock_symbol_count, 1);
  addGlobalFunc(ctx, "__flox_feed_clock_symbol_at", js_feed_clock_symbol_at, 2);
  addGlobalFunc(ctx, "__flox_feed_clock_tick", js_feed_clock_tick, 3);
  addGlobalFunc(ctx, "__flox_feed_clock_last_fired", js_feed_clock_last_fired, 1);
  addGlobalFunc(ctx, "__flox_feed_clock_last_triggered_by",
                js_feed_clock_last_triggered_by, 1);
  addGlobalFunc(ctx, "__flox_feed_clock_last_seen_at", js_feed_clock_last_seen_at, 2);
  addGlobalFunc(ctx, "__flox_feed_clock_staleness_at", js_feed_clock_staleness_at, 2);
  addGlobalFunc(ctx, "__flox_feed_clock_reset", js_feed_clock_reset, 1);

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
  addGlobalFunc(ctx, "__flox_indicator_adf", js_indicator_adf, 3);
  addGlobalFunc(ctx, "__flox_indicator_autocorrelation", js_indicator_autocorrelation, 3);

  // Targets (forward-looking labels)
  addGlobalFunc(ctx, "__flox_target_future_return", js_target_future_return, 2);
  addGlobalFunc(ctx, "__flox_target_future_ctc_volatility", js_target_future_ctc_volatility, 2);
  addGlobalFunc(ctx, "__flox_target_future_linear_slope", js_target_future_linear_slope, 2);

  // IndicatorGraph (batch)
  addGlobalFunc(ctx, "__flox_graph_create", js_graph_create, 0);
  addGlobalFunc(ctx, "__flox_graph_destroy", js_graph_destroy, 1);
  addGlobalFunc(ctx, "__flox_graph_set_bars", js_graph_set_bars, 6);
  addGlobalFunc(ctx, "__flox_graph_add_node", js_graph_add_node, 5);
  addGlobalFunc(ctx, "__flox_graph_require", js_graph_require, 3);
  addGlobalFunc(ctx, "__flox_graph_get", js_graph_get, 3);
  addGlobalFunc(ctx, "__flox_graph_close", js_graph_close, 2);
  addGlobalFunc(ctx, "__flox_graph_high", js_graph_high, 2);
  addGlobalFunc(ctx, "__flox_graph_low", js_graph_low, 2);
  addGlobalFunc(ctx, "__flox_graph_volume", js_graph_volume, 2);
  addGlobalFunc(ctx, "__flox_graph_invalidate", js_graph_invalidate, 2);
  addGlobalFunc(ctx, "__flox_graph_invalidate_all", js_graph_invalidate_all, 1);

  // StreamingIndicatorGraph
  addGlobalFunc(ctx, "__flox_streaming_create", js_streaming_create, 0);
  addGlobalFunc(ctx, "__flox_streaming_destroy", js_streaming_destroy, 1);
  addGlobalFunc(ctx, "__flox_streaming_add_node", js_streaming_add_node, 5);
  addGlobalFunc(ctx, "__flox_streaming_step", js_streaming_step, 6);
  addGlobalFunc(ctx, "__flox_streaming_current", js_streaming_current, 3);
  addGlobalFunc(ctx, "__flox_streaming_bar_count", js_streaming_bar_count, 2);
  addGlobalFunc(ctx, "__flox_streaming_reset", js_streaming_reset, 2);
  addGlobalFunc(ctx, "__flox_streaming_reset_all", js_streaming_reset_all, 1);
  addGlobalFunc(ctx, "__flox_streaming_close", js_streaming_close, 2);
  addGlobalFunc(ctx, "__flox_streaming_high", js_streaming_high, 2);
  addGlobalFunc(ctx, "__flox_streaming_low", js_streaming_low, 2);
  addGlobalFunc(ctx, "__flox_streaming_volume", js_streaming_volume, 2);

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
  addGlobalFunc(ctx, "__flox_simulated_executor_create", js_executor_create, 0);
  addGlobalFunc(ctx, "__flox_simulated_executor_destroy", js_executor_destroy, 1);
  addGlobalFunc(ctx, "__flox_simulated_executor_submit", js_executor_submit, 7);
  addGlobalFunc(ctx, "__flox_simulated_executor_submit_ex", js_executor_submit_ex, 10);
  addGlobalFunc(ctx, "__flox_simulated_executor_submit_bracket",
                js_executor_submit_bracket, 13);
  addGlobalFunc(ctx, "__flox_simulated_executor_cancel_bracket",
                js_executor_cancel_bracket, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_bracket_state",
                js_executor_bracket_state, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_bracket_child_arm_mode",
                js_executor_set_bracket_child_arm_mode, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_on_bar", js_executor_on_bar, 3);
  addGlobalFunc(ctx, "__flox_simulated_executor_on_trade", js_executor_on_trade, 4);
  addGlobalFunc(ctx, "__flox_simulated_executor_advance_clock", js_executor_advance, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_fill_count", js_executor_fill_count, 1);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_default_slippage",
                js_executor_set_default_slippage, 6);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_symbol_slippage",
                js_executor_set_symbol_slippage, 7);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_queue_model", js_executor_set_queue_model, 3);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_submit_ack", js_executor_set_submit_ack, 3);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_cancel_ack", js_executor_set_cancel_ack, 3);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_replace_ack", js_executor_set_replace_ack, 3);
  addGlobalFunc(ctx, "__flox_simulated_executor_apply_latency_profile",
                js_executor_apply_latency_profile, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_stp_mode",
                js_executor_set_stp_mode, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_fok_mode",
                js_executor_set_fok_mode, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_fok_mode",
                js_executor_fok_mode, 1);
  addGlobalFunc(ctx, "__flox_latency_distribution_create", js_latency_dist_create, 0);
  addGlobalFunc(ctx, "__flox_latency_distribution_destroy", js_latency_dist_destroy, 1);
  addGlobalFunc(ctx, "__flox_latency_distribution_set_constant",
                js_latency_dist_set_constant, 2);
  addGlobalFunc(ctx, "__flox_latency_distribution_set_uniform",
                js_latency_dist_set_uniform, 3);
  addGlobalFunc(ctx, "__flox_latency_distribution_set_lognormal",
                js_latency_dist_set_lognormal, 3);
  addGlobalFunc(ctx, "__flox_latency_distribution_set_empirical",
                js_latency_dist_set_empirical, 2);
  addGlobalFunc(ctx, "__flox_latency_distribution_set_burst_correlation",
                js_latency_dist_set_burst, 2);
  addGlobalFunc(ctx, "__flox_latency_distribution_median_ns",
                js_latency_dist_median_ns, 1);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_submit_ack_latency_distribution",
                js_executor_set_submit_ack_dist, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_cancel_ack_latency_distribution",
                js_executor_set_cancel_ack_dist, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_replace_ack_latency_distribution",
                js_executor_set_replace_ack_dist, 2);
  addGlobalFunc(ctx, "__flox_rate_limit_policy_create", js_rate_limit_create, 0);
  addGlobalFunc(ctx, "__flox_rate_limit_policy_destroy", js_rate_limit_destroy, 1);
  addGlobalFunc(ctx, "__flox_rate_limit_policy_add_bucket_family",
                js_rate_limit_add_bucket_family, 9);
  addGlobalFunc(ctx, "__flox_rate_limit_policy_add_bucket",
                js_rate_limit_add_bucket, 7);
  addGlobalFunc(ctx, "__flox_rate_limit_policy_set_ban", js_rate_limit_set_ban, 3);
  addGlobalFunc(ctx, "__flox_rate_limit_policy_load_profile",
                js_rate_limit_load_profile, 2);
  addGlobalFunc(ctx, "__flox_rate_limit_policy_ban_until_ns",
                js_rate_limit_ban_until_ns, 1);
  addGlobalFunc(ctx, "__flox_rate_limit_policy_consecutive_rejects",
                js_rate_limit_consecutive_rejects, 1);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_rate_limit_policy",
                js_executor_set_rate_limit_policy, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_clear_rate_limit_policy",
                js_executor_clear_rate_limit_policy, 1);
  addGlobalFunc(ctx, "__flox_venue_availability_create",
                js_venue_availability_create, 0);
  addGlobalFunc(ctx, "__flox_venue_availability_destroy",
                js_venue_availability_destroy, 1);
  addGlobalFunc(ctx, "__flox_venue_availability_schedule_outage",
                js_venue_availability_schedule_outage, 5);
  addGlobalFunc(ctx, "__flox_venue_availability_auto_random_outages",
                js_venue_availability_auto_random_outages, 5);
  addGlobalFunc(ctx, "__flox_venue_availability_is_up",
                js_venue_availability_is_up, 2);
  addGlobalFunc(ctx, "__flox_simulated_executor_set_venue_availability",
                js_executor_set_venue_availability, 2);
  addGlobalFunc(ctx, "__flox_fee_schedule_create", js_fee_schedule_create, 0);
  addGlobalFunc(ctx, "__flox_fee_schedule_destroy", js_fee_schedule_destroy, 1);
  addGlobalFunc(ctx, "__flox_fee_schedule_add_tier", js_fee_schedule_add_tier, 4);
  addGlobalFunc(ctx, "__flox_fee_schedule_load_profile",
                js_fee_schedule_load_profile, 2);
  addGlobalFunc(ctx, "__flox_liquidation_engine_create",
                js_liquidation_engine_create, 0);
  addGlobalFunc(ctx, "__flox_liquidation_engine_destroy",
                js_liquidation_engine_destroy, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_add_tier",
                js_liquidation_engine_add_tier, 3);
  addGlobalFunc(ctx, "__flox_liquidation_engine_set_insurance_fund_capital",
                js_liquidation_engine_set_insurance_fund_capital, 2);
  addGlobalFunc(ctx, "__flox_liquidation_engine_insurance_fund_balance",
                js_liquidation_engine_insurance_fund_balance, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_set_adl_enabled",
                js_liquidation_engine_set_adl_enabled, 2);
  addGlobalFunc(ctx, "__flox_liquidation_engine_set_adl_ranking",
                js_liquidation_engine_set_adl_ranking, 2);
  addGlobalFunc(ctx, "__flox_liquidation_engine_adl_ranking",
                js_liquidation_engine_adl_ranking, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_set_liquidation_slippage_bps",
                js_liquidation_engine_set_liquidation_slippage_bps, 2);
  addGlobalFunc(ctx, "__flox_liquidation_engine_open_position",
                js_liquidation_engine_open_position, 6);
  addGlobalFunc(ctx, "__flox_liquidation_engine_close_position",
                js_liquidation_engine_close_position, 3);
  addGlobalFunc(ctx, "__flox_liquidation_engine_on_mark",
                js_liquidation_engine_on_mark, 3);
  addGlobalFunc(ctx, "__flox_liquidation_engine_liquidations_count",
                js_liquidation_engine_liquidations_count, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_insurance_payments_count",
                js_liquidation_engine_insurance_payments_count, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_adl_closeouts_count",
                js_liquidation_engine_adl_closeouts_count, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_load_profile",
                js_liquidation_engine_load_profile, 2);
  addGlobalFunc(ctx, "__flox_liquidation_engine_set_executor",
                js_liquidation_engine_set_executor, 2);
  addGlobalFunc(ctx, "__flox_liquidation_engine_ticks_to_first_adl",
                js_liquidation_engine_ticks_to_first_adl, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_reset_stats",
                js_liquidation_engine_reset_stats, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_deficits_paid_by_fund",
                js_liquidation_engine_deficits_paid_by_fund, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_deficits_paid_by_adl",
                js_liquidation_engine_deficits_paid_by_adl, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_cascade_sizes",
                js_liquidation_engine_cascade_sizes, 1);
  addGlobalFunc(ctx, "__flox_liquidation_engine_fund_balance_history",
                js_liquidation_engine_fund_balance_history, 1);
  addGlobalFunc(ctx, "__flox_fee_schedule_record_fill", js_fee_schedule_record_fill, 3);
  addGlobalFunc(ctx, "__flox_fee_schedule_fee_for", js_fee_schedule_fee_for, 4);
  addGlobalFunc(ctx, "__flox_fee_schedule_current_tier",
                js_fee_schedule_current_tier, 1);
  addGlobalFunc(ctx, "__flox_fee_schedule_rolling_notional",
                js_fee_schedule_rolling_notional, 1);
  addGlobalFunc(ctx, "__flox_fee_schedule_reset_rolling", js_fee_schedule_reset, 1);
  addGlobalFunc(ctx, "__flox_funding_schedule_create", js_funding_schedule_create, 0);
  addGlobalFunc(ctx, "__flox_funding_schedule_destroy", js_funding_schedule_destroy, 1);
  addGlobalFunc(ctx, "__flox_funding_schedule_set_constant",
                js_funding_schedule_set_constant, 3);
  addGlobalFunc(ctx, "__flox_funding_schedule_load_profile",
                js_funding_schedule_load_profile, 2);
  addGlobalFunc(ctx, "__flox_funding_schedule_set_constant_rate",
                js_funding_schedule_set_constant_rate, 2);
  addGlobalFunc(ctx, "__flox_funding_schedule_reset", js_funding_schedule_reset, 1);
  addGlobalFunc(ctx, "__flox_funding_schedule_tick", js_funding_schedule_tick, 5);
  addGlobalFunc(ctx, "__flox_simulated_executor_on_trade_qty", js_executor_on_trade_qty, 5);
  addGlobalFunc(ctx, "__flox_simulated_executor_on_best_levels", js_executor_on_best_levels, 6);

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

  // Delta book compression
  addGlobalFunc(ctx, "__flox_delta_book_encoder_create", js_delta_book_encoder_create, 1);
  addGlobalFunc(ctx, "__flox_delta_book_encoder_destroy", js_delta_book_encoder_destroy, 1);
  addGlobalFunc(ctx, "__flox_delta_book_encoder_encode", js_delta_book_encoder_encode, 4);
  addGlobalFunc(ctx, "__flox_delta_book_replayer_create", js_delta_book_replayer_create, 0);
  addGlobalFunc(ctx, "__flox_delta_book_replayer_destroy", js_delta_book_replayer_destroy, 1);
  addGlobalFunc(ctx, "__flox_delta_book_replayer_apply", js_delta_book_replayer_apply, 5);

  addGlobalFunc(ctx, "__flox_run_recorder_create", js_run_recorder_create, 1);
  addGlobalFunc(ctx, "__flox_run_recorder_destroy", js_run_recorder_destroy, 1);
  addGlobalFunc(ctx, "__flox_run_recorder_add_tape_ref", js_run_recorder_add_tape_ref, 2);
  addGlobalFunc(ctx, "__flox_run_recorder_set_run_ended_ns", js_run_recorder_set_run_ended_ns, 2);
  addGlobalFunc(ctx, "__flox_run_recorder_write_signal", js_run_recorder_write_signal, 2);
  addGlobalFunc(ctx, "__flox_run_recorder_write_order_event", js_run_recorder_write_order_event, 2);
  addGlobalFunc(ctx, "__flox_run_recorder_write_fill", js_run_recorder_write_fill, 2);
  addGlobalFunc(ctx, "__flox_run_recorder_close", js_run_recorder_close, 1);
  addGlobalFunc(ctx, "__flox_run_reader_open", js_run_reader_open, 1);
  addGlobalFunc(ctx, "__flox_run_reader_close", js_run_reader_close, 1);
  addGlobalFunc(ctx, "__flox_run_reader_strategy_id", js_run_reader_strategy_id, 1);
  addGlobalFunc(ctx, "__flox_run_reader_run_started_ns", js_run_reader_run_started_ns, 1);
  addGlobalFunc(ctx, "__flox_run_reader_run_ended_ns", js_run_reader_run_ended_ns, 1);
  addGlobalFunc(ctx, "__flox_run_reader_signals", js_run_reader_signals, 1);
  addGlobalFunc(ctx, "__flox_run_reader_orders", js_run_reader_orders, 1);
  addGlobalFunc(ctx, "__flox_run_reader_fills", js_run_reader_fills, 1);

  // Order group (multi-leg state machine; QuickJS class wraps these in
  // quickjs/flox/order_group.js so all four bindings share the engine).
  addGlobalFunc(ctx, "__flox_order_group_create", js_order_group_create, 2);
  addGlobalFunc(ctx, "__flox_order_group_destroy", js_order_group_destroy, 1);
  addGlobalFunc(ctx, "__flox_order_group_add_market_leg", js_order_group_add_market_leg, 4);
  addGlobalFunc(ctx, "__flox_order_group_add_limit_leg", js_order_group_add_limit_leg, 5);
  addGlobalFunc(ctx, "__flox_order_group_leg_count", js_order_group_leg_count, 1);
  addGlobalFunc(ctx, "__flox_order_group_leg_state", js_order_group_leg_state, 2);
  addGlobalFunc(ctx, "__flox_order_group_leg_filled", js_order_group_leg_filled, 2);
  addGlobalFunc(ctx, "__flox_order_group_leg_order_id", js_order_group_leg_order_id, 2);
  addGlobalFunc(ctx, "__flox_order_group_record_submit", js_order_group_record_submit, 3);
  addGlobalFunc(ctx, "__flox_order_group_record_fill", js_order_group_record_fill, 3);
  addGlobalFunc(ctx, "__flox_order_group_record_cancel", js_order_group_record_cancel, 2);
  addGlobalFunc(ctx, "__flox_order_group_record_failure", js_order_group_record_failure, 2);
  addGlobalFunc(ctx, "__flox_order_group_record_replace_accepted",
                js_order_group_record_replace_accepted, 3);
  addGlobalFunc(ctx, "__flox_order_group_record_replace_rejected",
                js_order_group_record_replace_rejected, 2);
  addGlobalFunc(ctx, "__flox_order_group_find_leg_by_order_id",
                js_order_group_find_leg_by_order_id, 2);
  addGlobalFunc(ctx, "__flox_order_group_state", js_order_group_state, 1);
  addGlobalFunc(ctx, "__flox_order_group_recommended_actions",
                js_order_group_recommended_actions, 1);
  addGlobalFunc(ctx, "__flox_order_group_mark_action_dispatched",
                js_order_group_mark_action_dispatched, 3);
  addGlobalFunc(ctx, "__flox_order_group_set_risk_limits", js_order_group_set_risk_limits, 4);
  addGlobalFunc(ctx, "__flox_order_group_precheck_submission",
                js_order_group_precheck_submission, 3);
  addGlobalFunc(ctx, "__flox_order_group_set_pair_latency_budget_ns",
                js_order_group_set_pair_latency_budget_ns, 2);
  addGlobalFunc(ctx, "__flox_order_group_pair_latency_decision",
                js_order_group_pair_latency_decision, 4);

  // Live queue position estimator
  addGlobalFunc(ctx, "__flox_live_queue_position_create",
                js_live_queue_position_create, 0);
  addGlobalFunc(ctx, "__flox_live_queue_position_destroy",
                js_live_queue_position_destroy, 1);
  addGlobalFunc(ctx, "__flox_live_queue_position_set_confidence_half_life_ns",
                js_live_queue_position_set_half_life, 2);
  addGlobalFunc(ctx, "__flox_live_queue_position_set_shrink_factor",
                js_live_queue_position_set_shrink_factor, 2);
  addGlobalFunc(ctx, "__flox_live_queue_position_on_order_placed",
                js_live_queue_position_on_order_placed, 8);
  addGlobalFunc(ctx, "__flox_live_queue_position_on_order_cancelled",
                js_live_queue_position_on_order_cancelled, 3);
  addGlobalFunc(ctx, "__flox_live_queue_position_on_order_filled",
                js_live_queue_position_on_order_filled, 4);
  addGlobalFunc(ctx, "__flox_live_queue_position_on_trade",
                js_live_queue_position_on_trade, 5);
  addGlobalFunc(ctx, "__flox_live_queue_position_on_trade_with_flag",
                js_live_queue_position_on_trade_with_flag, 6);
  addGlobalFunc(ctx, "__flox_live_queue_position_set_hidden_order_policy",
                js_live_queue_position_set_hidden_order_policy, 2);
  addGlobalFunc(ctx, "__flox_live_queue_position_on_level_update",
                js_live_queue_position_on_level_update, 6);
  addGlobalFunc(ctx, "__flox_live_queue_position_snapshot",
                js_live_queue_position_snapshot, 3);
  addGlobalFunc(ctx, "__flox_live_queue_position_tracked_count",
                js_live_queue_position_tracked_count, 1);

  // Bar dispatch recorder (cross-binding parity test fixture)
  addGlobalFunc(ctx, "__flox_bar_dispatch_recorder_create", js_bar_dispatch_recorder_create, 0);
  addGlobalFunc(ctx, "__flox_bar_dispatch_recorder_destroy", js_bar_dispatch_recorder_destroy, 1);
  addGlobalFunc(ctx, "__flox_bar_dispatch_recorder_add_time_seconds",
                js_bar_dispatch_recorder_add_time_seconds, 2);
  addGlobalFunc(ctx, "__flox_bar_dispatch_recorder_on_trade",
                js_bar_dispatch_recorder_on_trade, 5);
  addGlobalFunc(ctx, "__flox_bar_dispatch_recorder_finalize",
                js_bar_dispatch_recorder_finalize, 1);
  addGlobalFunc(ctx, "__flox_bar_dispatch_recorder_count", js_bar_dispatch_recorder_count, 1);
  addGlobalFunc(ctx, "__flox_bar_dispatch_recorder_type_at",
                js_bar_dispatch_recorder_type_at, 2);
  addGlobalFunc(ctx, "__flox_bar_dispatch_recorder_param_at",
                js_bar_dispatch_recorder_param_at, 2);

  // Tape diff
  addGlobalFunc(ctx, "__flox_tape_diff", js_tape_diff, 3);

  // Execution algos (TWAP / VWAP / Iceberg / POV)
  addGlobalFunc(ctx, "__flox_exec_twap_create", js_exec_twap_create, 8);
  addGlobalFunc(ctx, "__flox_exec_vwap_create", js_exec_vwap_create, 6);
  addGlobalFunc(ctx, "__flox_exec_iceberg_create", js_exec_iceberg_create, 6);
  addGlobalFunc(ctx, "__flox_exec_pov_create", js_exec_pov_create, 7);
  addGlobalFunc(ctx, "__flox_exec_destroy", js_exec_destroy, 1);
  addGlobalFunc(ctx, "__flox_exec_step", js_exec_step, 2);
  addGlobalFunc(ctx, "__flox_exec_report_fill", js_exec_report_fill, 2);
  addGlobalFunc(ctx, "__flox_exec_observe_volume", js_exec_observe_volume, 2);
  addGlobalFunc(ctx, "__flox_exec_submitted_qty", js_exec_submitted_qty, 1);
  addGlobalFunc(ctx, "__flox_exec_filled_qty", js_exec_filled_qty, 1);
  addGlobalFunc(ctx, "__flox_exec_remaining_qty", js_exec_remaining_qty, 1);
  addGlobalFunc(ctx, "__flox_exec_is_done", js_exec_is_done, 1);

  // Portfolio risk aggregator
  addGlobalFunc(ctx, "__flox_portfolio_risk_create", js_portfolio_risk_create, 2);
  addGlobalFunc(ctx, "__flox_portfolio_risk_destroy", js_portfolio_risk_destroy, 1);
  addGlobalFunc(ctx, "__flox_portfolio_risk_update", js_portfolio_risk_update, 3);
  addGlobalFunc(ctx, "__flox_portfolio_risk_remove", js_portfolio_risk_remove, 2);
  addGlobalFunc(ctx, "__flox_portfolio_risk_reset", js_portfolio_risk_reset_kill_switch, 1);
  addGlobalFunc(ctx, "__flox_portfolio_risk_check_order", js_portfolio_risk_check_order, 4);
  addGlobalFunc(ctx, "__flox_portfolio_risk_snapshot", js_portfolio_risk_snapshot, 1);

  // Latency models (Phase 1 sampling primitive)
  addGlobalFunc(ctx, "__flox_lat_constant_create", js_lat_constant_create, 3);
  addGlobalFunc(ctx, "__flox_lat_gaussian_create", js_lat_gaussian_create, 7);
  addGlobalFunc(ctx, "__flox_lat_exponential_create", js_lat_exponential_create, 4);
  addGlobalFunc(ctx, "__flox_lat_empirical_create", js_lat_empirical_create, 4);
  addGlobalFunc(ctx, "__flox_lat_destroy", js_lat_destroy, 1);
  addGlobalFunc(ctx, "__flox_lat_feed_delay", js_lat_feed_delay, 1);
  addGlobalFunc(ctx, "__flox_lat_order_delay", js_lat_order_delay, 1);
  addGlobalFunc(ctx, "__flox_lat_fill_delay", js_lat_fill_delay, 1);
  addGlobalFunc(ctx, "__flox_lat_sample", js_lat_sample, 1);
  addGlobalFunc(ctx, "__flox_lat_reset", js_lat_reset, 2);

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

  // OrderJourneyTracer
  addGlobalFunc(ctx, "__flox_ojt_create", js_ojt_create, 4);
  addGlobalFunc(ctx, "__flox_ojt_destroy", js_ojt_destroy, 1);
  addGlobalFunc(ctx, "__flox_ojt_order_count", js_ojt_order_count, 1);
  addGlobalFunc(ctx, "__flox_ojt_record_count", js_ojt_record_count, 1);
  addGlobalFunc(ctx, "__flox_ojt_median_ack", js_ojt_median_ack, 1);
  addGlobalFunc(ctx, "__flox_ojt_median_ttf", js_ojt_median_ttf, 1);
  addGlobalFunc(ctx, "__flox_ojt_maker_ratio", js_ojt_maker_ratio, 1);
  addGlobalFunc(ctx, "__flox_ojt_cancel_race", js_ojt_cancel_race, 1);
  addGlobalFunc(ctx, "__flox_ojt_result", js_ojt_result, 1);
  addGlobalFunc(ctx, "__flox_ojt_journey", js_ojt_journey, 2);
  addGlobalFunc(ctx, "__flox_ojt_clear", js_ojt_clear, 1);

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
  addGlobalFunc(ctx, "__flox_dw_write_book", js_dw_write_book, 10);
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
  addGlobalFunc(ctx, "__flox_dr_read_bbo", js_dr_read_bbo, 2);
  addGlobalFunc(ctx, "__flox_dr_read_book_updates", js_dr_read_book_updates, 1);
  addGlobalFunc(ctx, "__flox_dr_read_trades_from", js_dr_read_trades_from, 3);
  addGlobalFunc(ctx, "__flox_dr_read_bbo_from", js_dr_read_bbo_from, 3);
  addGlobalFunc(ctx, "__flox_dr_read_book_updates_from", js_dr_read_book_updates_from, 2);

  // MergedTapeReader
  addGlobalFunc(ctx, "__flox_mtr_create", js_mtr_create, 4);
  addGlobalFunc(ctx, "__flox_mtr_destroy", js_mtr_destroy, 1);
  addGlobalFunc(ctx, "__flox_mtr_symbol_count", js_mtr_symbol_count, 1);
  addGlobalFunc(ctx, "__flox_mtr_get_symbols", js_mtr_get_symbols, 1);
  addGlobalFunc(ctx, "__flox_mtr_tape_count", js_mtr_tape_count, 1);
  addGlobalFunc(ctx, "__flox_mtr_get_tape_stats", js_mtr_get_tape_stats, 1);
  addGlobalFunc(ctx, "__flox_mtr_time_range", js_mtr_time_range, 1);
  addGlobalFunc(ctx, "__flox_mtr_count_trades", js_mtr_count_trades, 1);
  addGlobalFunc(ctx, "__flox_mtr_read_trades", js_mtr_read_trades, 2);
  addGlobalFunc(ctx, "__flox_mtr_count_books", js_mtr_count_books, 1);
  addGlobalFunc(ctx, "__flox_mtr_read_books", js_mtr_read_books, 1);

  // BinaryLogRecorderHook
  addGlobalFunc(ctx, "__flox_blrh_create", js_blrh_create, 6);
  addGlobalFunc(ctx, "__flox_blrh_destroy", js_blrh_destroy, 1);
  addGlobalFunc(ctx, "__flox_blrh_as_recorder", js_blrh_as_recorder, 1);
  addGlobalFunc(ctx, "__flox_blrh_add_symbol", js_blrh_add_symbol, 7);
  addGlobalFunc(ctx, "__flox_blrh_flush", js_blrh_flush, 1);
  addGlobalFunc(ctx, "__flox_blrh_stats", js_blrh_stats, 1);

  // MarketDataRecorder handle drivers — push lifecycle + trade events
  // directly through the recorder callbacks, no Runner required.
  addGlobalFunc(ctx, "__flox_recorder_on_start", js_recorder_on_start, 1);
  addGlobalFunc(ctx, "__flox_recorder_on_stop", js_recorder_on_stop, 1);
  addGlobalFunc(ctx, "__flox_recorder_on_trade", js_recorder_on_trade, 6);

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
