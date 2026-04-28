/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/capi/flox_capi.h"
#include "flox/capi/bridge_strategy.h"
#include "flox/engine/symbol_registry.h"

#include "flox/aggregator/bar.h"
#include "flox/aggregator/policies/tick_bar_policy.h"
#include "flox/aggregator/policies/time_bar_policy.h"
#include "flox/aggregator/policies/volume_bar_policy.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/replay/binary_format_v1.h"

#include "flox/indicator/adx.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/bollinger.h"
#include "flox/indicator/cci.h"
#include "flox/indicator/chop.h"
#include "flox/indicator/correlation.h"
#include "flox/indicator/cvd.h"
#include "flox/indicator/dema.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/indicator_pipeline.h"
#include "flox/indicator/kama.h"
#include "flox/indicator/kurtosis.h"
#include "flox/indicator/macd.h"
#include "flox/indicator/obv.h"
#include "flox/indicator/parkinson_vol.h"
#include "flox/indicator/rma.h"
#include "flox/indicator/rogers_satchell_vol.h"
#include "flox/indicator/rolling_zscore.h"
#include "flox/indicator/rsi.h"
#include "flox/indicator/shannon_entropy.h"
#include "flox/indicator/skewness.h"
#include "flox/indicator/slope.h"
#include "flox/indicator/sma.h"
#include "flox/indicator/stochastic.h"
#include "flox/indicator/vwap.h"

#include "flox/aggregator/custom/footprint_bar.h"
#include "flox/aggregator/custom/market_profile.h"
#include "flox/aggregator/custom/volume_profile.h"
#include "flox/aggregator/policies/heikin_ashi_bar_policy.h"
#include "flox/aggregator/policies/range_bar_policy.h"
#include "flox/aggregator/policies/renko_bar_policy.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/book/bus/trade_bus.h"
#include "flox/book/composite_book_matrix.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/book/l3/l3_order_book.h"
#include "flox/execution/order_tracker.h"
#include "flox/position/position_group.h"
#include "flox/position/position_tracker.h"
#include "flox/replay/market_data_recorder.h"
#include "flox/replay/ops/partitioner.h"
#include "flox/replay/ops/segment_ops.h"
#include "flox/replay/ops/validator.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"
#include "flox/strategy/abstract_signal_handler.h"
#include "flox/util/memory/pool.h"

#include <random>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <span>
#include <sstream>
#include <vector>

using namespace flox;

// ============================================================
// Internal helpers
// ============================================================

static BridgeStrategy* toStrategy(FloxStrategyHandle h)
{
  return static_cast<BridgeStrategy*>(h);
}

static SymbolRegistry* toRegistry(FloxRegistryHandle h)
{
  return static_cast<SymbolRegistry*>(h);
}

// ============================================================
// Registry
// ============================================================

FloxRegistryHandle flox_registry_create(void)
{
  return static_cast<FloxRegistryHandle>(new SymbolRegistry());
}

void flox_registry_destroy(FloxRegistryHandle registry)
{
  delete toRegistry(registry);
}

uint32_t flox_registry_add_symbol(FloxRegistryHandle registry, const char* exchange,
                                  const char* name, double tick_size)
{
  auto* reg = toRegistry(registry);
  SymbolInfo info;
  info.exchange = exchange;
  info.symbol = name;
  info.tickSize = Price::fromDouble(tick_size);
  return reg->registerSymbol(info);
}

uint8_t flox_registry_get_symbol_id(FloxRegistryHandle registry, const char* exchange,
                                    const char* name, uint32_t* id_out)
{
  auto* reg = toRegistry(registry);
  auto result = reg->getSymbolId(exchange, name);
  if (result.has_value())
  {
    *id_out = result.value();
    return 1;
  }
  return 0;
}

uint8_t flox_registry_get_symbol_name(FloxRegistryHandle registry, uint32_t symbol_id,
                                      char* exchange_out, size_t exchange_len, char* name_out,
                                      size_t name_len)
{
  auto* reg = toRegistry(registry);
  auto info = reg->getSymbolInfo(symbol_id);
  if (!info.has_value())
  {
    return 0;
  }
  std::strncpy(exchange_out, info->exchange.c_str(), exchange_len - 1);
  exchange_out[exchange_len - 1] = '\0';
  std::strncpy(name_out, info->symbol.c_str(), name_len - 1);
  name_out[name_len - 1] = '\0';
  return 1;
}

uint32_t flox_registry_symbol_count(FloxRegistryHandle registry)
{
  return static_cast<uint32_t>(toRegistry(registry)->size());
}

// ============================================================
// Strategy lifecycle
// ============================================================

FloxStrategyHandle flox_strategy_create(uint32_t id, const uint32_t* symbols,
                                        uint32_t num_symbols, FloxRegistryHandle registry,
                                        FloxStrategyCallbacks callbacks)
{
  auto* reg = toRegistry(registry);
  std::vector<SymbolId> syms(symbols, symbols + num_symbols);
  auto* strat = new BridgeStrategy(static_cast<SubscriberId>(id), std::move(syms), *reg, callbacks);
  return static_cast<FloxStrategyHandle>(strat);
}

void flox_strategy_destroy(FloxStrategyHandle strategy)
{
  delete toStrategy(strategy);
}

// ============================================================
// Signal emission
// ============================================================

uint64_t flox_emit_market_buy(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw)
{
  return toStrategy(s)->publicEmitMarketBuy(symbol, Quantity::fromRaw(qty_raw));
}

uint64_t flox_emit_market_sell(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw)
{
  return toStrategy(s)->publicEmitMarketSell(symbol, Quantity::fromRaw(qty_raw));
}

uint64_t flox_emit_limit_buy(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                             int64_t qty_raw)
{
  return toStrategy(s)->publicEmitLimitBuy(symbol, Price::fromRaw(price_raw),
                                           Quantity::fromRaw(qty_raw));
}

uint64_t flox_emit_limit_sell(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                              int64_t qty_raw)
{
  return toStrategy(s)->publicEmitLimitSell(symbol, Price::fromRaw(price_raw),
                                            Quantity::fromRaw(qty_raw));
}

void flox_emit_cancel(FloxStrategyHandle s, uint64_t order_id)
{
  toStrategy(s)->publicEmitCancel(order_id);
}

void flox_emit_cancel_all(FloxStrategyHandle s, uint32_t symbol)
{
  toStrategy(s)->publicEmitCancelAll(symbol);
}

void flox_emit_modify(FloxStrategyHandle s, uint64_t order_id, int64_t new_price_raw,
                      int64_t new_qty_raw)
{
  toStrategy(s)->publicEmitModify(order_id, Price::fromRaw(new_price_raw),
                                  Quantity::fromRaw(new_qty_raw));
}

uint64_t flox_emit_stop_market(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                               int64_t trigger_raw, int64_t qty_raw)
{
  return toStrategy(s)->publicEmitStopMarket(symbol, side == 0 ? Side::BUY : Side::SELL,
                                             Price::fromRaw(trigger_raw),
                                             Quantity::fromRaw(qty_raw));
}

uint64_t flox_emit_stop_limit(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                              int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw)
{
  return toStrategy(s)->publicEmitStopLimit(symbol, side == 0 ? Side::BUY : Side::SELL,
                                            Price::fromRaw(trigger_raw),
                                            Price::fromRaw(limit_raw),
                                            Quantity::fromRaw(qty_raw));
}

uint64_t flox_emit_take_profit_market(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                      int64_t trigger_raw, int64_t qty_raw)
{
  return toStrategy(s)->publicEmitTakeProfitMarket(symbol, side == 0 ? Side::BUY : Side::SELL,
                                                   Price::fromRaw(trigger_raw),
                                                   Quantity::fromRaw(qty_raw));
}

uint64_t flox_emit_trailing_stop(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                 int64_t offset_raw, int64_t qty_raw)
{
  return toStrategy(s)->publicEmitTrailingStop(symbol, side == 0 ? Side::BUY : Side::SELL,
                                               Price::fromRaw(offset_raw),
                                               Quantity::fromRaw(qty_raw));
}

uint64_t flox_emit_trailing_stop_percent(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                         int32_t callback_bps, int64_t qty_raw)
{
  return toStrategy(s)->publicEmitTrailingStopPercent(
      symbol, side == 0 ? Side::BUY : Side::SELL, callback_bps, Quantity::fromRaw(qty_raw));
}

uint64_t flox_emit_take_profit_limit(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                     int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw)
{
  return toStrategy(s)->publicEmitTakeProfitLimit(
      symbol, side == 0 ? Side::BUY : Side::SELL, Price::fromRaw(trigger_raw),
      Price::fromRaw(limit_raw), Quantity::fromRaw(qty_raw));
}

uint64_t flox_emit_limit_buy_tif(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                 int64_t qty_raw, uint8_t time_in_force)
{
  return toStrategy(s)->publicEmitLimitBuyTif(
      symbol, Price::fromRaw(price_raw), Quantity::fromRaw(qty_raw),
      static_cast<TimeInForce>(time_in_force));
}

uint64_t flox_emit_limit_sell_tif(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                  int64_t qty_raw, uint8_t time_in_force)
{
  return toStrategy(s)->publicEmitLimitSellTif(
      symbol, Price::fromRaw(price_raw), Quantity::fromRaw(qty_raw),
      static_cast<TimeInForce>(time_in_force));
}

uint64_t flox_emit_close_position(FloxStrategyHandle s, uint32_t symbol)
{
  return toStrategy(s)->publicEmitClosePosition(symbol);
}

int32_t flox_get_order_status(FloxStrategyHandle s, uint64_t order_id)
{
  auto status = toStrategy(s)->getOrderStatus(order_id);
  return status ? static_cast<int32_t>(*status) : -1;
}

// ============================================================
// Context queries
// ============================================================

int64_t flox_position_raw(FloxStrategyHandle s, uint32_t symbol)
{
  return toStrategy(s)->position(symbol).raw();
}

int64_t flox_last_trade_price_raw(FloxStrategyHandle s, uint32_t symbol)
{
  return toStrategy(s)->ctx(symbol).lastTradePrice.raw();
}

int64_t flox_best_bid_raw(FloxStrategyHandle s, uint32_t symbol)
{
  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  return bid ? bid->raw() : 0;
}

int64_t flox_best_ask_raw(FloxStrategyHandle s, uint32_t symbol)
{
  auto ask = toStrategy(s)->ctx(symbol).book.bestAsk();
  return ask ? ask->raw() : 0;
}

int64_t flox_mid_price_raw(FloxStrategyHandle s, uint32_t symbol)
{
  auto mid = toStrategy(s)->ctx(symbol).mid();
  return mid ? mid->raw() : 0;
}

void flox_get_symbol_context(FloxStrategyHandle s, uint32_t symbol, FloxSymbolContext* out)
{
  const auto& c = toStrategy(s)->ctx(symbol);
  out->symbol_id = c.symbolId;
  out->position_raw = c.position.raw();
  out->avg_entry_price_raw = c.avgEntryPrice.raw();
  out->last_trade_price_raw = c.lastTradePrice.raw();
  out->last_update_ns = c.lastUpdateNs;

  auto bid = c.book.bestBid();
  auto ask = c.book.bestAsk();
  out->book.bid_price_raw = bid ? bid->raw() : 0;
  out->book.bid_qty_raw = 0;
  out->book.ask_price_raw = ask ? ask->raw() : 0;
  out->book.ask_qty_raw = 0;
  auto mid = c.mid();
  out->book.mid_raw = mid ? mid->raw() : 0;
  auto spread = c.bookSpread();
  out->book.spread_raw = spread ? spread->raw() : 0;
}

// ============================================================
// Fixed-point conversion
// ============================================================

int64_t flox_price_from_double(double value)
{
  return Price::fromDouble(value).raw();
}

double flox_price_to_double(int64_t raw)
{
  return Price::fromRaw(raw).toDouble();
}

int64_t flox_quantity_from_double(double value)
{
  return Quantity::fromDouble(value).raw();
}

double flox_quantity_to_double(int64_t raw)
{
  return Quantity::fromRaw(raw).toDouble();
}

// ============================================================
// Indicators
// ============================================================

void flox_indicator_ema(const double* input, size_t len, size_t period, double* output)
{
  indicator::EMA ema(period);
  ema.compute(std::span<const double>(input, len), std::span<double>(output, len));
}

void flox_indicator_sma(const double* input, size_t len, size_t period, double* output)
{
  indicator::SMA sma(period);
  sma.compute(std::span<const double>(input, len), std::span<double>(output, len));
}

void flox_indicator_rsi(const double* input, size_t len, size_t period, double* output)
{
  indicator::RSI rsi(period);
  rsi.compute(std::span<const double>(input, len), std::span<double>(output, len));
}

void flox_indicator_atr(const double* high, const double* low, const double* close, size_t len,
                        size_t period, double* output)
{
  indicator::ATR atr(period);
  atr.compute(std::span<const double>(high, len), std::span<const double>(low, len),
              std::span<const double>(close, len), std::span<double>(output, len));
}

void flox_indicator_macd(const double* input, size_t len, size_t fast_period, size_t slow_period,
                         size_t signal_period, double* macd_out, double* signal_out,
                         double* hist_out)
{
  indicator::MACD macd(fast_period, slow_period, signal_period);
  macd.compute(std::span<const double>(input, len), std::span<double>(macd_out, len),
               std::span<double>(signal_out, len), std::span<double>(hist_out, len));
}

void flox_indicator_bollinger(const double* input, size_t len, size_t period, double multiplier,
                              double* upper, double* middle, double* lower)
{
  indicator::Bollinger bb(period, multiplier);
  bb.compute(std::span<const double>(input, len), std::span<double>(upper, len),
             std::span<double>(middle, len), std::span<double>(lower, len));
}

void flox_indicator_rma(const double* input, size_t len, size_t period, double* output)
{
  indicator::RMA rma(period);
  rma.compute(std::span<const double>(input, len), std::span<double>(output, len));
}

void flox_indicator_dema(const double* input, size_t len, size_t period, double* output)
{
  indicator::DEMA dema(period);
  auto result = dema.compute(std::span<const double>(input, len));
  std::copy(result.begin(), result.end(), output);
}

void flox_indicator_tema(const double* input, size_t len, size_t period, double* output)
{
  indicator::TEMA tema(period);
  auto result = tema.compute(std::span<const double>(input, len));
  std::copy(result.begin(), result.end(), output);
}

void flox_indicator_kama(const double* input, size_t len, size_t period, size_t fast, size_t slow,
                         double* output)
{
  indicator::KAMA kama(period, fast, slow);
  auto result = kama.compute(std::span<const double>(input, len));
  std::copy(result.begin(), result.end(), output);
}

void flox_indicator_slope(const double* input, size_t len, size_t length, double* output)
{
  indicator::Slope slope(length);
  slope.compute(std::span<const double>(input, len), std::span<double>(output, len));
}

void flox_indicator_adx(const double* high, const double* low, const double* close, size_t len,
                        size_t period, double* adx_out, double* plus_di_out, double* minus_di_out)
{
  indicator::ADX adx(period);
  auto result = adx.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                            std::span<const double>(close, len));
  std::copy(result.adx.begin(), result.adx.end(), adx_out);
  std::copy(result.plus_di.begin(), result.plus_di.end(), plus_di_out);
  std::copy(result.minus_di.begin(), result.minus_di.end(), minus_di_out);
}

void flox_indicator_cci(const double* high, const double* low, const double* close, size_t len,
                        size_t period, double* output)
{
  indicator::CCI cci(period);
  auto result = cci.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                            std::span<const double>(close, len));
  std::copy(result.begin(), result.end(), output);
}

void flox_indicator_stochastic(const double* high, const double* low, const double* close,
                               size_t len, size_t k_period, size_t d_period, double* k_out,
                               double* d_out)
{
  indicator::Stochastic stoch(k_period, d_period);
  auto result =
      stoch.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                    std::span<const double>(close, len));
  std::copy(result.k.begin(), result.k.end(), k_out);
  std::copy(result.d.begin(), result.d.end(), d_out);
}

void flox_indicator_chop(const double* high, const double* low, const double* close, size_t len,
                         size_t period, double* output)
{
  indicator::CHOP chop(period);
  auto result = chop.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                             std::span<const double>(close, len));
  std::copy(result.begin(), result.end(), output);
}

void flox_indicator_obv(const double* close, const double* volume, size_t len, double* output)
{
  indicator::OBV obv;
  obv.compute(std::span<const double>(close, len), std::span<const double>(volume, len),
              std::span<double>(output, len));
}

void flox_indicator_vwap(const double* close, const double* volume, size_t len, size_t window,
                         double* output)
{
  indicator::VWAP vwap(window);
  auto result =
      vwap.compute(std::span<const double>(close, len), std::span<const double>(volume, len));
  std::copy(result.begin(), result.end(), output);
}

void flox_indicator_cvd(const double* open, const double* high, const double* low,
                        const double* close, const double* volume, size_t len, double* output)
{
  indicator::CVD cvd;
  auto result = cvd.compute(
      std::span<const double>(open, len), std::span<const double>(high, len),
      std::span<const double>(low, len), std::span<const double>(close, len),
      std::span<const double>(volume, len));
  std::copy(result.begin(), result.end(), output);
}

void flox_indicator_skewness(const double* input, size_t len, size_t period, double* output)
{
  indicator::Skewness ind(period);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
}

void flox_indicator_kurtosis(const double* input, size_t len, size_t period, double* output)
{
  indicator::Kurtosis ind(period);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
}

void flox_indicator_parkinson_vol(const double* high, const double* low, size_t len, size_t period,
                                  double* output)
{
  indicator::ParkinsonVol ind(period);
  ind.compute(std::span<const double>(high, len), std::span<const double>(low, len),
              std::span<double>(output, len));
}

void flox_indicator_rogers_satchell_vol(const double* open, const double* high, const double* low,
                                        const double* close, size_t len, size_t period,
                                        double* output)
{
  indicator::RogersSatchellVol ind(period);
  ind.compute(std::span<const double>(open, len), std::span<const double>(high, len),
              std::span<const double>(low, len), std::span<const double>(close, len),
              std::span<double>(output, len));
}

void flox_indicator_rolling_zscore(const double* input, size_t len, size_t period, double* output)
{
  indicator::RollingZScore ind(period);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
}

void flox_indicator_shannon_entropy(const double* input, size_t len, size_t period, size_t bins,
                                    double* output)
{
  indicator::ShannonEntropy ind(period, bins);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
}

void flox_indicator_correlation(const double* x, const double* y, size_t len, size_t period,
                                double* output)
{
  indicator::Correlation ind(period);
  ind.compute(std::span<const double>(x, len), std::span<const double>(y, len),
              std::span<double>(output, len));
}

// ============================================================
// IndicatorGraph (batch) — handle-based wrapper.
// ============================================================

namespace
{

struct FloxGraphImpl
{
  indicator::IndicatorGraph graph;
  std::unordered_map<uint32_t, std::vector<Bar>> barStorage;
};

}  // namespace

FloxIndicatorGraphHandle flox_indicator_graph_create(void)
{
  return new FloxGraphImpl();
}

void flox_indicator_graph_destroy(FloxIndicatorGraphHandle g)
{
  delete static_cast<FloxGraphImpl*>(g);
}

void flox_indicator_graph_set_bars(FloxIndicatorGraphHandle g, uint32_t symbol,
                                   const double* close, const double* high, const double* low,
                                   const double* volume, size_t len)
{
  auto* impl = static_cast<FloxGraphImpl*>(g);
  std::vector<Bar> bars(len);
  for (size_t i = 0; i < len; ++i)
  {
    bars[i].open = Price::fromDouble(close[i]);
    bars[i].high = Price::fromDouble(high ? high[i] : close[i]);
    bars[i].low = Price::fromDouble(low ? low[i] : close[i]);
    bars[i].close = Price::fromDouble(close[i]);
    bars[i].volume = volume ? Volume::fromDouble(volume[i]) : Volume{};
  }
  impl->barStorage[symbol] = std::move(bars);
  impl->graph.setBars(static_cast<SymbolId>(symbol),
                      std::span<const Bar>(impl->barStorage[symbol]));
}

void flox_indicator_graph_add_node(FloxIndicatorGraphHandle g, const char* name,
                                   const char* const* deps, size_t num_deps,
                                   FloxGraphNodeFn fn, void* user_data)
{
  auto* impl = static_cast<FloxGraphImpl*>(g);
  std::vector<std::string> depList;
  depList.reserve(num_deps);
  for (size_t i = 0; i < num_deps; ++i)
  {
    depList.emplace_back(deps[i]);
  }
  impl->graph.addNode(name, std::move(depList),
                      [g, fn, user_data](indicator::IndicatorGraph&, SymbolId sym)
                      {
                        size_t outLen = 0;
                        const double* p = fn(user_data, g, static_cast<uint32_t>(sym), &outLen);
                        if (!p)
                        {
                          return std::vector<double>{};
                        }
                        return std::vector<double>(p, p + outLen);
                      });
}

const double* flox_indicator_graph_require(FloxIndicatorGraphHandle g, uint32_t symbol,
                                           const char* name, size_t* len_out)
{
  auto* impl = static_cast<FloxGraphImpl*>(g);
  try
  {
    const auto& v = impl->graph.require(static_cast<SymbolId>(symbol), name);
    if (len_out)
    {
      *len_out = v.size();
    }
    return v.data();
  }
  catch (...)
  {
    if (len_out)
    {
      *len_out = 0;
    }
    return nullptr;
  }
}

const double* flox_indicator_graph_get(FloxIndicatorGraphHandle g, uint32_t symbol,
                                       const char* name, size_t* len_out)
{
  auto* impl = static_cast<FloxGraphImpl*>(g);
  const auto* v = impl->graph.get(static_cast<SymbolId>(symbol), name);
  if (!v)
  {
    if (len_out)
    {
      *len_out = 0;
    }
    return nullptr;
  }
  if (len_out)
  {
    *len_out = v->size();
  }
  return v->data();
}

const double* flox_indicator_graph_close(FloxIndicatorGraphHandle g, uint32_t symbol,
                                         size_t* len_out)
{
  auto* impl = static_cast<FloxGraphImpl*>(g);
  const auto& v = impl->graph.close(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
}

const double* flox_indicator_graph_high(FloxIndicatorGraphHandle g, uint32_t symbol,
                                        size_t* len_out)
{
  auto* impl = static_cast<FloxGraphImpl*>(g);
  const auto& v = impl->graph.high(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
}

const double* flox_indicator_graph_low(FloxIndicatorGraphHandle g, uint32_t symbol,
                                       size_t* len_out)
{
  auto* impl = static_cast<FloxGraphImpl*>(g);
  const auto& v = impl->graph.low(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
}

const double* flox_indicator_graph_volume(FloxIndicatorGraphHandle g, uint32_t symbol,
                                          size_t* len_out)
{
  auto* impl = static_cast<FloxGraphImpl*>(g);
  const auto& v = impl->graph.volume(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
}

void flox_indicator_graph_invalidate(FloxIndicatorGraphHandle g, uint32_t symbol)
{
  static_cast<FloxGraphImpl*>(g)->graph.invalidate(static_cast<SymbolId>(symbol));
}

void flox_indicator_graph_invalidate_all(FloxIndicatorGraphHandle g)
{
  static_cast<FloxGraphImpl*>(g)->graph.invalidateAll();
}

// ============================================================
// Order book
// ============================================================

struct FloxBookImpl
{
  NLevelOrderBook<8192> book;
  explicit FloxBookImpl(double tickSize) : book(Price::fromDouble(tickSize)) {}

  void applyUpdate(const double* bp, const double* bq, size_t bl, const double* ap,
                   const double* aq, size_t al, BookUpdateType type)
  {
    std::byte buf[32768];
    std::pmr::monotonic_buffer_resource res(buf, sizeof(buf));
    BookUpdateEvent ev(&res);
    ev.update.type = type;
    ev.update.bids.reserve(bl);
    for (size_t i = 0; i < bl; ++i)
    {
      ev.update.bids.push_back({Price::fromDouble(bp[i]), Quantity::fromDouble(bq[i])});
    }
    ev.update.asks.reserve(al);
    for (size_t i = 0; i < al; ++i)
    {
      ev.update.asks.push_back({Price::fromDouble(ap[i]), Quantity::fromDouble(aq[i])});
    }
    book.applyBookUpdate(ev);
  }
};

FloxBookHandle flox_book_create(double tick_size)
{
  return new FloxBookImpl(tick_size);
}

void flox_book_destroy(FloxBookHandle book)
{
  delete static_cast<FloxBookImpl*>(book);
}

void flox_book_apply_snapshot(FloxBookHandle h, const double* bp, const double* bq, size_t bl,
                              const double* ap, const double* aq, size_t al)
{
  static_cast<FloxBookImpl*>(h)->applyUpdate(bp, bq, bl, ap, aq, al, BookUpdateType::SNAPSHOT);
}

void flox_book_apply_delta(FloxBookHandle h, const double* bp, const double* bq, size_t bl,
                           const double* ap, const double* aq, size_t al)
{
  static_cast<FloxBookImpl*>(h)->applyUpdate(bp, bq, bl, ap, aq, al, BookUpdateType::DELTA);
}

uint8_t flox_book_best_bid(FloxBookHandle h, double* price_out)
{
  auto bid = static_cast<FloxBookImpl*>(h)->book.bestBid();
  if (bid)
  {
    *price_out = bid->toDouble();
    return 1;
  }
  return 0;
}

uint8_t flox_book_best_ask(FloxBookHandle h, double* price_out)
{
  auto ask = static_cast<FloxBookImpl*>(h)->book.bestAsk();
  if (ask)
  {
    *price_out = ask->toDouble();
    return 1;
  }
  return 0;
}

uint8_t flox_book_mid(FloxBookHandle h, double* price_out)
{
  auto& book = static_cast<FloxBookImpl*>(h)->book;
  auto mid = book.mid();
  if (mid)
  {
    *price_out = mid->toDouble();
    return 1;
  }
  return 0;
}

uint8_t flox_book_spread(FloxBookHandle h, double* spread_out)
{
  auto& book = static_cast<FloxBookImpl*>(h)->book;
  auto sp = book.spread();
  if (sp)
  {
    *spread_out = sp->toDouble();
    return 1;
  }
  return 0;
}

double flox_book_bid_at_price(FloxBookHandle h, double price)
{
  return static_cast<FloxBookImpl*>(h)->book.bidAtPrice(Price::fromDouble(price)).toDouble();
}

double flox_book_ask_at_price(FloxBookHandle h, double price)
{
  return static_cast<FloxBookImpl*>(h)->book.askAtPrice(Price::fromDouble(price)).toDouble();
}

uint8_t flox_book_is_crossed(FloxBookHandle h)
{
  return static_cast<FloxBookImpl*>(h)->book.isCrossed() ? 1 : 0;
}

void flox_book_clear(FloxBookHandle h)
{
  static_cast<FloxBookImpl*>(h)->book.clear();
}

// ============================================================
// Simulated executor
// ============================================================

struct FloxExecutorImpl
{
  SimulatedClock clock;
  SimulatedExecutor executor;
  FloxExecutorImpl() : executor(clock) {}
};

FloxExecutorHandle flox_executor_create(void)
{
  return new FloxExecutorImpl();
}

void flox_executor_destroy(FloxExecutorHandle executor)
{
  delete static_cast<FloxExecutorImpl*>(executor);
}

void flox_executor_submit_order(FloxExecutorHandle h, uint64_t id, uint8_t side, double price,
                                double quantity, uint8_t order_type, uint32_t symbol)
{
  auto* impl = static_cast<FloxExecutorImpl*>(h);
  Order order{};
  order.id = id;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(quantity);
  order.type = static_cast<OrderType>(order_type);
  order.symbol = symbol;
  impl->executor.submitOrder(order);
}

void flox_executor_cancel_order(FloxExecutorHandle h, uint64_t order_id)
{
  static_cast<FloxExecutorImpl*>(h)->executor.cancelOrder(order_id);
}

void flox_executor_cancel_all(FloxExecutorHandle h, uint32_t symbol)
{
  static_cast<FloxExecutorImpl*>(h)->executor.cancelAllOrders(symbol);
}

void flox_executor_on_bar(FloxExecutorHandle h, uint32_t symbol, double close_price)
{
  static_cast<FloxExecutorImpl*>(h)->executor.onBar(symbol, Price::fromDouble(close_price));
}

void flox_executor_on_trade(FloxExecutorHandle h, uint32_t symbol, double price, uint8_t is_buy)
{
  static_cast<FloxExecutorImpl*>(h)->executor.onTrade(symbol, Price::fromDouble(price),
                                                      is_buy != 0);
}

void flox_executor_advance_clock(FloxExecutorHandle h, int64_t timestamp_ns)
{
  static_cast<FloxExecutorImpl*>(h)->clock.advanceTo(timestamp_ns);
}

uint32_t flox_executor_fill_count(FloxExecutorHandle h)
{
  return static_cast<uint32_t>(static_cast<FloxExecutorImpl*>(h)->executor.fills().size());
}

// ============================================================
// Bar aggregation
// ============================================================

template <typename Policy>
static uint32_t doAggregateC(Policy& policy, const int64_t* ts, const double* px,
                             const double* qty, const uint8_t* ib, size_t n, FloxBar* bars_out,
                             uint32_t max_bars)
{
  uint32_t count = 0;
  Bar currentBar;
  bool initialized = false;

  for (size_t i = 0; i < n; ++i)
  {
    TradeEvent trade;
    trade.trade.price = Price::fromDouble(px[i]);
    trade.trade.quantity = Quantity::fromDouble(qty[i]);
    trade.trade.isBuy = (ib[i] != 0);
    trade.trade.exchangeTsNs = ts[i];
    trade.trade.symbol = 1;
    trade.trade.instrument = InstrumentType::Spot;

    if (!initialized)
    {
      policy.initBar(trade, currentBar);
      initialized = true;
      continue;
    }

    if (policy.shouldClose(trade, currentBar))
    {
      if (count < max_bars)
      {
        bars_out[count] = {currentBar.startTime.time_since_epoch().count(),
                           currentBar.endTime.time_since_epoch().count(),
                           currentBar.open.raw(),
                           currentBar.high.raw(),
                           currentBar.low.raw(),
                           currentBar.close.raw(),
                           currentBar.volume.raw(),
                           currentBar.buyVolume.raw(),
                           static_cast<uint32_t>(currentBar.tradeCount.raw())};
      }
      count++;
      policy.initBar(trade, currentBar);
      continue;
    }
    policy.update(trade, currentBar);
  }
  return count;
}

uint32_t flox_aggregate_time_bars(const int64_t* ts, const double* px, const double* qty,
                                  const uint8_t* ib, size_t len, double interval_seconds,
                                  FloxBar* bars_out, uint32_t max_bars)
{
  TimeBarPolicy policy(std::chrono::nanoseconds(
      static_cast<int64_t>(interval_seconds * 1'000'000'000.0)));
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
}

uint32_t flox_aggregate_tick_bars(const int64_t* ts, const double* px, const double* qty,
                                  const uint8_t* ib, size_t len, uint32_t tick_count,
                                  FloxBar* bars_out, uint32_t max_bars)
{
  TickBarPolicy policy(tick_count);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
}

uint32_t flox_aggregate_volume_bars(const int64_t* ts, const double* px, const double* qty,
                                    const uint8_t* ib, size_t len, double volume_threshold,
                                    FloxBar* bars_out, uint32_t max_bars)
{
  auto policy = VolumeBarPolicy::fromDouble(volume_threshold);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
}

// ============================================================
// Position tracking
// ============================================================

FloxPositionTrackerHandle flox_position_tracker_create(uint8_t cost_basis)
{
  auto method = static_cast<CostBasisMethod>(cost_basis);
  return new PositionTracker(0, method);
}

void flox_position_tracker_destroy(FloxPositionTrackerHandle tracker)
{
  delete static_cast<PositionTracker*>(tracker);
}

void flox_position_tracker_on_fill(FloxPositionTrackerHandle h, uint32_t symbol, uint8_t side,
                                   double price, double quantity)
{
  auto* tracker = static_cast<PositionTracker*>(h);
  Order order{};
  order.symbol = symbol;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(quantity);
  order.id = 0;
  tracker->onOrderFilled(order);
}

double flox_position_tracker_position(FloxPositionTrackerHandle h, uint32_t symbol)
{
  return static_cast<PositionTracker*>(h)->getPosition(symbol).toDouble();
}

double flox_position_tracker_avg_entry(FloxPositionTrackerHandle h, uint32_t symbol)
{
  return static_cast<PositionTracker*>(h)->getAvgEntryPrice(symbol).toDouble();
}

double flox_position_tracker_realized_pnl(FloxPositionTrackerHandle h, uint32_t symbol)
{
  return static_cast<PositionTracker*>(h)->getRealizedPnl(symbol).toDouble();
}

double flox_position_tracker_total_pnl(FloxPositionTrackerHandle h)
{
  return static_cast<PositionTracker*>(h)->getTotalRealizedPnl().toDouble();
}

// ============================================================
// Volume profile
// ============================================================

FloxVolumeProfileHandle flox_volume_profile_create(double tick_size)
{
  auto* vp = new VolumeProfile<>();
  vp->setTickSize(Price::fromDouble(tick_size));
  return vp;
}

void flox_volume_profile_destroy(FloxVolumeProfileHandle profile)
{
  delete static_cast<VolumeProfile<>*>(profile);
}

void flox_volume_profile_add_trade(FloxVolumeProfileHandle h, double price, double quantity,
                                   uint8_t is_buy)
{
  TradeEvent te;
  te.trade.price = Price::fromDouble(price);
  te.trade.quantity = Quantity::fromDouble(quantity);
  te.trade.isBuy = (is_buy != 0);
  static_cast<VolumeProfile<>*>(h)->addTrade(te);
}

double flox_volume_profile_poc(FloxVolumeProfileHandle h)
{
  return static_cast<VolumeProfile<>*>(h)->poc().toDouble();
}

double flox_volume_profile_vah(FloxVolumeProfileHandle h)
{
  return static_cast<VolumeProfile<>*>(h)->valueAreaHigh().toDouble();
}

double flox_volume_profile_val(FloxVolumeProfileHandle h)
{
  return static_cast<VolumeProfile<>*>(h)->valueAreaLow().toDouble();
}

double flox_volume_profile_total_volume(FloxVolumeProfileHandle h)
{
  return static_cast<VolumeProfile<>*>(h)->totalVolume().toDouble();
}

double flox_volume_profile_total_delta(FloxVolumeProfileHandle h)
{
  return static_cast<VolumeProfile<>*>(h)->totalDelta().toDouble();
}

uint32_t flox_volume_profile_num_levels(FloxVolumeProfileHandle h)
{
  return static_cast<uint32_t>(static_cast<VolumeProfile<>*>(h)->numLevels());
}

void flox_volume_profile_clear(FloxVolumeProfileHandle h)
{
  static_cast<VolumeProfile<>*>(h)->clear();
}

// ============================================================
// Footprint bar
// ============================================================

FloxFootprintHandle flox_footprint_create(double tick_size)
{
  auto* fp = new FootprintBar<>();
  fp->setTickSize(Price::fromDouble(tick_size));
  return fp;
}

void flox_footprint_destroy(FloxFootprintHandle footprint)
{
  delete static_cast<FootprintBar<>*>(footprint);
}

void flox_footprint_add_trade(FloxFootprintHandle h, double price, double quantity,
                              uint8_t is_buy)
{
  TradeEvent te;
  te.trade.price = Price::fromDouble(price);
  te.trade.quantity = Quantity::fromDouble(quantity);
  te.trade.isBuy = (is_buy != 0);
  static_cast<FootprintBar<>*>(h)->addTrade(te);
}

double flox_footprint_total_delta(FloxFootprintHandle h)
{
  return static_cast<FootprintBar<>*>(h)->totalDelta().toDouble();
}

double flox_footprint_total_volume(FloxFootprintHandle h)
{
  return static_cast<FootprintBar<>*>(h)->totalVolume().toDouble();
}

uint32_t flox_footprint_num_levels(FloxFootprintHandle h)
{
  return static_cast<uint32_t>(static_cast<FootprintBar<>*>(h)->numLevels());
}

void flox_footprint_clear(FloxFootprintHandle h)
{
  static_cast<FootprintBar<>*>(h)->clear();
}

// ============================================================
// Statistics
// ============================================================

double flox_stat_correlation(const double* x, const double* y, size_t len)
{
  double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0, sumY2 = 0;
  for (size_t i = 0; i < len; i++)
  {
    sumX += x[i];
    sumY += y[i];
    sumXY += x[i] * y[i];
    sumX2 += x[i] * x[i];
    sumY2 += y[i] * y[i];
  }
  double n = static_cast<double>(len);
  double num = n * sumXY - sumX * sumY;
  double den = std::sqrt((n * sumX2 - sumX * sumX) * (n * sumY2 - sumY * sumY));
  return den != 0 ? num / den : 0;
}

double flox_stat_profit_factor(const double* pnl, size_t len)
{
  double gross_profit = 0, gross_loss = 0;
  for (size_t i = 0; i < len; i++)
  {
    if (pnl[i] > 0)
    {
      gross_profit += pnl[i];
    }
    else
    {
      gross_loss -= pnl[i];
    }
  }
  return gross_loss > 0 ? gross_profit / gross_loss : (gross_profit > 0 ? 1e9 : 0);
}

double flox_stat_win_rate(const double* pnl, size_t len)
{
  if (len == 0)
  {
    return 0;
  }
  size_t wins = 0;
  for (size_t i = 0; i < len; i++)
  {
    if (pnl[i] > 0)
    {
      wins++;
    }
  }
  return static_cast<double>(wins) / static_cast<double>(len);
}

// ============================================================
// Order tracker
// ============================================================

FloxOrderTrackerHandle flox_order_tracker_create(void)
{
  return new OrderTracker();
}

void flox_order_tracker_destroy(FloxOrderTrackerHandle tracker)
{
  delete static_cast<OrderTracker*>(tracker);
}

uint8_t flox_order_tracker_on_submitted(FloxOrderTrackerHandle h, uint64_t order_id,
                                        uint32_t symbol, uint8_t side, double price, double qty)
{
  Order order{};
  order.id = order_id;
  order.symbol = symbol;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(qty);
  return static_cast<OrderTracker*>(h)->onSubmitted(order, "", "") ? 1 : 0;
}

uint8_t flox_order_tracker_on_filled(FloxOrderTrackerHandle h, uint64_t order_id, double fill_qty)
{
  return static_cast<OrderTracker*>(h)->onFilled(order_id, Quantity::fromDouble(fill_qty)) ? 1
                                                                                           : 0;
}

uint8_t flox_order_tracker_on_canceled(FloxOrderTrackerHandle h, uint64_t order_id)
{
  return static_cast<OrderTracker*>(h)->onCanceled(order_id) ? 1 : 0;
}

uint8_t flox_order_tracker_is_active(FloxOrderTrackerHandle h, uint64_t order_id)
{
  return static_cast<OrderTracker*>(h)->isActive(order_id) ? 1 : 0;
}

uint32_t flox_order_tracker_active_count(FloxOrderTrackerHandle h)
{
  return static_cast<uint32_t>(static_cast<OrderTracker*>(h)->activeOrderCount());
}

uint32_t flox_order_tracker_total_count(FloxOrderTrackerHandle h)
{
  return static_cast<uint32_t>(static_cast<OrderTracker*>(h)->totalOrderCount());
}

void flox_order_tracker_prune(FloxOrderTrackerHandle h)
{
  static_cast<OrderTracker*>(h)->pruneTerminal();
}

// ============================================================
// Position group tracker
// ============================================================

FloxPositionGroupHandle flox_position_group_create(void)
{
  return new PositionGroupTracker();
}

void flox_position_group_destroy(FloxPositionGroupHandle h)
{
  delete static_cast<PositionGroupTracker*>(h);
}

uint64_t flox_position_group_open(FloxPositionGroupHandle h, uint64_t order_id, uint32_t symbol,
                                  uint8_t side, double price, double qty)
{
  return static_cast<PositionGroupTracker*>(h)->openPosition(
      order_id, symbol, side == 0 ? Side::BUY : Side::SELL, Price::fromDouble(price),
      Quantity::fromDouble(qty));
}

void flox_position_group_close(FloxPositionGroupHandle h, uint64_t position_id, double exit_price)
{
  static_cast<PositionGroupTracker*>(h)->closePosition(position_id,
                                                       Price::fromDouble(exit_price));
}

void flox_position_group_partial_close(FloxPositionGroupHandle h, uint64_t position_id, double qty,
                                       double exit_price)
{
  static_cast<PositionGroupTracker*>(h)->partialClose(position_id, Quantity::fromDouble(qty),
                                                      Price::fromDouble(exit_price));
}

double flox_position_group_net_position(FloxPositionGroupHandle h, uint32_t symbol)
{
  return static_cast<PositionGroupTracker*>(h)->netPosition(symbol).toDouble();
}

double flox_position_group_realized_pnl(FloxPositionGroupHandle h, uint32_t symbol)
{
  return static_cast<PositionGroupTracker*>(h)->realizedPnl(symbol).toDouble();
}

double flox_position_group_total_pnl(FloxPositionGroupHandle h)
{
  return static_cast<PositionGroupTracker*>(h)->totalRealizedPnl().toDouble();
}

uint32_t flox_position_group_open_count(FloxPositionGroupHandle h, uint32_t symbol)
{
  return static_cast<uint32_t>(
      static_cast<PositionGroupTracker*>(h)->openPositionCount(symbol));
}

void flox_position_group_prune(FloxPositionGroupHandle h)
{
  static_cast<PositionGroupTracker*>(h)->pruneClosedPositions();
}

// ============================================================
// Market profile
// ============================================================

FloxMarketProfileHandle flox_market_profile_create(double tick_size, uint32_t period_minutes,
                                                   int64_t session_start_ns)
{
  auto* mp = new MarketProfile<>();
  mp->setTickSize(Price::fromDouble(tick_size));
  mp->setPeriodDuration(std::chrono::minutes(period_minutes));
  mp->setSessionStart(static_cast<uint64_t>(session_start_ns));
  return mp;
}

void flox_market_profile_destroy(FloxMarketProfileHandle h)
{
  delete static_cast<MarketProfile<>*>(h);
}

void flox_market_profile_add_trade(FloxMarketProfileHandle h, int64_t timestamp_ns, double price,
                                   double qty, uint8_t is_buy)
{
  TradeEvent te;
  te.trade.price = Price::fromDouble(price);
  te.trade.quantity = Quantity::fromDouble(qty);
  te.trade.isBuy = (is_buy != 0);
  te.trade.exchangeTsNs = timestamp_ns;
  static_cast<MarketProfile<>*>(h)->addTrade(te);
}

double flox_market_profile_poc(FloxMarketProfileHandle h)
{
  return static_cast<MarketProfile<>*>(h)->poc().toDouble();
}

double flox_market_profile_vah(FloxMarketProfileHandle h)
{
  return static_cast<MarketProfile<>*>(h)->valueAreaHigh().toDouble();
}

double flox_market_profile_val(FloxMarketProfileHandle h)
{
  return static_cast<MarketProfile<>*>(h)->valueAreaLow().toDouble();
}

double flox_market_profile_ib_high(FloxMarketProfileHandle h)
{
  return static_cast<MarketProfile<>*>(h)->initialBalanceHigh().toDouble();
}

double flox_market_profile_ib_low(FloxMarketProfileHandle h)
{
  return static_cast<MarketProfile<>*>(h)->initialBalanceLow().toDouble();
}

uint8_t flox_market_profile_is_poor_high(FloxMarketProfileHandle h)
{
  return static_cast<MarketProfile<>*>(h)->isPoorHigh() ? 1 : 0;
}

uint8_t flox_market_profile_is_poor_low(FloxMarketProfileHandle h)
{
  return static_cast<MarketProfile<>*>(h)->isPoorLow() ? 1 : 0;
}

uint32_t flox_market_profile_num_levels(FloxMarketProfileHandle h)
{
  return static_cast<uint32_t>(static_cast<MarketProfile<>*>(h)->numLevels());
}

void flox_market_profile_clear(FloxMarketProfileHandle h)
{
  static_cast<MarketProfile<>*>(h)->clear();
}

// ============================================================
// Composite book
// ============================================================

FloxCompositeBookHandle flox_composite_book_create(void)
{
  return new CompositeBookMatrix<>();
}

void flox_composite_book_destroy(FloxCompositeBookHandle h)
{
  delete static_cast<CompositeBookMatrix<>*>(h);
}

uint8_t flox_composite_book_best_bid(FloxCompositeBookHandle h, uint32_t symbol, double* price_out,
                                     double* qty_out)
{
  auto q = static_cast<CompositeBookMatrix<>*>(h)->bestBid(symbol);
  if (q.valid)
  {
    *price_out = Price::fromRaw(q.priceRaw).toDouble();
    *qty_out = Quantity::fromRaw(q.qtyRaw).toDouble();
    return 1;
  }
  return 0;
}

uint8_t flox_composite_book_best_ask(FloxCompositeBookHandle h, uint32_t symbol, double* price_out,
                                     double* qty_out)
{
  auto q = static_cast<CompositeBookMatrix<>*>(h)->bestAsk(symbol);
  if (q.valid)
  {
    *price_out = Price::fromRaw(q.priceRaw).toDouble();
    *qty_out = Quantity::fromRaw(q.qtyRaw).toDouble();
    return 1;
  }
  return 0;
}

uint8_t flox_composite_book_has_arb(FloxCompositeBookHandle h, uint32_t symbol)
{
  return static_cast<CompositeBookMatrix<>*>(h)->hasArbitrageOpportunity(symbol) ? 1 : 0;
}

void flox_composite_book_mark_stale(FloxCompositeBookHandle h, uint32_t exchange, uint32_t symbol)
{
  static_cast<CompositeBookMatrix<>*>(h)->markStale(exchange, symbol);
}

void flox_composite_book_check_staleness(FloxCompositeBookHandle h, int64_t now_ns,
                                         int64_t threshold_ns)
{
  static_cast<CompositeBookMatrix<>*>(h)->checkStaleness(now_ns, threshold_ns);
}

// ============================================================
// Executor fill access
// ============================================================

uint32_t flox_executor_get_fills(FloxExecutorHandle h, FloxFill* fills_out, uint32_t max_fills)
{
  auto& fills = static_cast<FloxExecutorImpl*>(h)->executor.fills();
  uint32_t count = static_cast<uint32_t>(std::min(fills.size(), static_cast<size_t>(max_fills)));
  for (uint32_t i = 0; i < count; i++)
  {
    fills_out[i].order_id = fills[i].orderId;
    fills_out[i].symbol = fills[i].symbol;
    fills_out[i].side = fills[i].side == Side::BUY ? 0 : 1;
    fills_out[i].price_raw = fills[i].price.raw();
    fills_out[i].quantity_raw = fills[i].quantity.raw();
    fills_out[i].timestamp_ns = static_cast<int64_t>(fills[i].timestampNs);
  }
  return count;
}

// ============================================================
// Additional bar aggregation
// ============================================================

uint32_t flox_aggregate_range_bars(const int64_t* ts, const double* px, const double* qty,
                                   const uint8_t* ib, size_t len, double range_size,
                                   FloxBar* bars_out, uint32_t max_bars)
{
  auto policy = RangeBarPolicy::fromDouble(range_size);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
}

uint32_t flox_aggregate_renko_bars(const int64_t* ts, const double* px, const double* qty,
                                   const uint8_t* ib, size_t len, double brick_size,
                                   FloxBar* bars_out, uint32_t max_bars)
{
  auto policy = RenkoBarPolicy::fromDouble(brick_size);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
}

// ============================================================
// L3 Order book
// ============================================================

FloxL3BookHandle flox_l3_book_create(void)
{
  return new L3OrderBook<>();
}

void flox_l3_book_destroy(FloxL3BookHandle h)
{
  delete static_cast<L3OrderBook<>*>(h);
}

int32_t flox_l3_book_add_order(FloxL3BookHandle h, uint64_t order_id, double price, double quantity,
                               uint8_t side)
{
  auto status = static_cast<L3OrderBook<>*>(h)->addOrder(
      order_id, Price::fromDouble(price), Quantity::fromDouble(quantity),
      side == 0 ? Side::BUY : Side::SELL);
  return static_cast<int32_t>(status);
}

int32_t flox_l3_book_remove_order(FloxL3BookHandle h, uint64_t order_id)
{
  auto status = static_cast<L3OrderBook<>*>(h)->removeOrder(order_id);
  return static_cast<int32_t>(status);
}

int32_t flox_l3_book_modify_order(FloxL3BookHandle h, uint64_t order_id, double new_qty)
{
  auto status =
      static_cast<L3OrderBook<>*>(h)->modifyOrder(order_id, Quantity::fromDouble(new_qty));
  return static_cast<int32_t>(status);
}

uint8_t flox_l3_book_best_bid(FloxL3BookHandle h, double* price_out)
{
  auto bid = static_cast<L3OrderBook<>*>(h)->bestBid();
  if (bid)
  {
    *price_out = bid->toDouble();
    return 1;
  }
  return 0;
}

uint8_t flox_l3_book_best_ask(FloxL3BookHandle h, double* price_out)
{
  auto ask = static_cast<L3OrderBook<>*>(h)->bestAsk();
  if (ask)
  {
    *price_out = ask->toDouble();
    return 1;
  }
  return 0;
}

double flox_l3_book_bid_at_price(FloxL3BookHandle h, double price)
{
  return static_cast<L3OrderBook<>*>(h)->bidAtPrice(Price::fromDouble(price)).toDouble();
}

double flox_l3_book_ask_at_price(FloxL3BookHandle h, double price)
{
  return static_cast<L3OrderBook<>*>(h)->askAtPrice(Price::fromDouble(price)).toDouble();
}

// ============================================================
// Data writer
// ============================================================

FloxDataWriterHandle flox_data_writer_create(const char* output_dir, uint64_t max_segment_mb,
                                             uint8_t exchange_id)
{
  replay::WriterConfig cfg;
  cfg.output_dir = output_dir;
  cfg.max_segment_bytes = max_segment_mb * 1024 * 1024;
  cfg.exchange_id = exchange_id;
  return new replay::BinaryLogWriter(cfg);
}

void flox_data_writer_destroy(FloxDataWriterHandle h)
{
  auto* w = static_cast<replay::BinaryLogWriter*>(h);
  w->close();
  delete w;
}

uint8_t flox_data_writer_write_trade(FloxDataWriterHandle h, int64_t exchange_ts_ns,
                                     int64_t recv_ts_ns, double price, double qty,
                                     uint64_t trade_id, uint32_t symbol_id, uint8_t side)
{
  replay::TradeRecord tr{};
  tr.exchange_ts_ns = exchange_ts_ns;
  tr.recv_ts_ns = recv_ts_ns;
  tr.price_raw = Price::fromDouble(price).raw();
  tr.qty_raw = Quantity::fromDouble(qty).raw();
  tr.trade_id = trade_id;
  tr.symbol_id = symbol_id;
  tr.side = side;
  return static_cast<replay::BinaryLogWriter*>(h)->writeTrade(tr) ? 1 : 0;
}

void flox_data_writer_flush(FloxDataWriterHandle h)
{
  static_cast<replay::BinaryLogWriter*>(h)->flush();
}

void flox_data_writer_close(FloxDataWriterHandle h)
{
  static_cast<replay::BinaryLogWriter*>(h)->close();
}

// ============================================================
// Data reader
// ============================================================

FloxDataReaderHandle flox_data_reader_create(const char* data_dir)
{
  replay::ReaderConfig cfg;
  cfg.data_dir = data_dir;
  return new replay::BinaryLogReader(cfg);
}

void flox_data_reader_destroy(FloxDataReaderHandle h)
{
  delete static_cast<replay::BinaryLogReader*>(h);
}

uint64_t flox_data_reader_count(FloxDataReaderHandle h)
{
  return static_cast<replay::BinaryLogReader*>(h)->count();
}

// ============================================================
// Order book level access
// ============================================================

uint32_t flox_book_get_bids(FloxBookHandle h, double* prices_out, double* qtys_out,
                            uint32_t max_levels)
{
  auto levels = static_cast<FloxBookImpl*>(h)->book.getBidLevels(max_levels);
  uint32_t count = static_cast<uint32_t>(levels.size());
  for (uint32_t i = 0; i < count; i++)
  {
    prices_out[i] = levels[i].price.toDouble();
    qtys_out[i] = levels[i].quantity.toDouble();
  }
  return count;
}

uint32_t flox_book_get_asks(FloxBookHandle h, double* prices_out, double* qtys_out,
                            uint32_t max_levels)
{
  auto levels = static_cast<FloxBookImpl*>(h)->book.getAskLevels(max_levels);
  uint32_t count = static_cast<uint32_t>(levels.size());
  for (uint32_t i = 0; i < count; i++)
  {
    prices_out[i] = levels[i].price.toDouble();
    qtys_out[i] = levels[i].quantity.toDouble();
  }
  return count;
}

// ============================================================
// Heikin-Ashi bar aggregation
// ============================================================

uint32_t flox_aggregate_heikin_ashi_bars(const int64_t* ts, const double* px, const double* qty,
                                         const uint8_t* ib, size_t len, double interval_seconds,
                                         FloxBar* bars_out, uint32_t max_bars)
{
  HeikinAshiBarPolicy policy(
      std::chrono::nanoseconds(static_cast<int64_t>(interval_seconds * 1e9)));
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
}

// ============================================================
// Additional stats
// ============================================================

double flox_stat_permutation_test(const double* group1, size_t len1, const double* group2,
                                  size_t len2, uint32_t num_permutations)
{
  auto mean = [](const double* d, size_t n)
  {
    double s = 0;
    for (size_t i = 0; i < n; i++)
    {
      s += d[i];
    }
    return n > 0 ? s / static_cast<double>(n) : 0.0;
  };

  double observed = std::abs(mean(group1, len1) - mean(group2, len2));

  std::vector<double> combined(len1 + len2);
  std::copy(group1, group1 + len1, combined.begin());
  std::copy(group2, group2 + len2, combined.begin() + len1);

  std::mt19937 rng(42);
  uint32_t count = 0;
  for (uint32_t i = 0; i < num_permutations; i++)
  {
    std::shuffle(combined.begin(), combined.end(), rng);
    double diff = std::abs(mean(combined.data(), len1) - mean(combined.data() + len1, len2));
    if (diff >= observed)
    {
      count++;
    }
  }
  return static_cast<double>(count) / static_cast<double>(num_permutations);
}

void flox_stat_bootstrap_ci(const double* data, size_t len, double confidence,
                            uint32_t num_samples, double* lower_out, double* median_out,
                            double* upper_out)
{
  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, len - 1);

  std::vector<double> means(num_samples);
  for (uint32_t s = 0; s < num_samples; s++)
  {
    double sum = 0;
    for (size_t i = 0; i < len; i++)
    {
      sum += data[dist(rng)];
    }
    means[s] = sum / static_cast<double>(len);
  }
  std::sort(means.begin(), means.end());

  double alpha = (1.0 - confidence) / 2.0;
  size_t lo = static_cast<size_t>(alpha * num_samples);
  size_t hi = static_cast<size_t>((1.0 - alpha) * num_samples);
  size_t mid = num_samples / 2;

  *lower_out = means[lo];
  *median_out = means[mid];
  *upper_out = means[std::min(hi, static_cast<size_t>(num_samples - 1))];
}

// ============================================================
// Segment operations
// ============================================================

uint8_t flox_segment_validate(const char* path)
{
  replay::SegmentValidator validator;
  auto result = validator.validate(path);
  return result.valid ? 1 : 0;
}

uint8_t flox_segment_merge(const char* input_dir, const char* output_path)
{
  replay::MergeConfig cfg;
  cfg.output_dir = output_path;
  auto result = replay::SegmentOps::mergeDirectory(input_dir, cfg);
  return result.segments_merged > 0 ? 1 : 0;
}

// ============================================================
// Backtest: slippage, queue, result, metrics, equity curve
// ============================================================

namespace
{
SlippageProfile makeSlippageProfile(int32_t model, int32_t ticks, double tick_size,
                                    double bps, double impact_coeff)
{
  SlippageProfile prof;
  prof.model = static_cast<SlippageModel>(model);
  prof.ticks = ticks;
  prof.tickSize = (tick_size > 0.0) ? Price::fromDouble(tick_size) : Price{};
  prof.bps = bps;
  prof.impactCoeff = impact_coeff;
  return prof;
}
}  // namespace

void flox_executor_set_default_slippage(FloxExecutorHandle h, int32_t model, int32_t ticks,
                                        double tick_size, double bps, double impact_coeff)
{
  static_cast<FloxExecutorImpl*>(h)->executor.setDefaultSlippage(
      makeSlippageProfile(model, ticks, tick_size, bps, impact_coeff));
}

void flox_executor_set_symbol_slippage(FloxExecutorHandle h, uint32_t symbol, int32_t model,
                                       int32_t ticks, double tick_size, double bps,
                                       double impact_coeff)
{
  static_cast<FloxExecutorImpl*>(h)->executor.setSymbolSlippage(
      symbol, makeSlippageProfile(model, ticks, tick_size, bps, impact_coeff));
}

void flox_executor_set_queue_model(FloxExecutorHandle h, int32_t model, uint32_t depth)
{
  static_cast<FloxExecutorImpl*>(h)->executor.setQueueModel(
      static_cast<QueueModel>(model), depth);
}

void flox_executor_on_trade_qty(FloxExecutorHandle h, uint32_t symbol, double price,
                                double quantity, uint8_t is_buy)
{
  static_cast<FloxExecutorImpl*>(h)->executor.onTrade(
      symbol, Price::fromDouble(price), Quantity::fromDouble(quantity), is_buy != 0);
}

void flox_executor_on_best_levels(FloxExecutorHandle h, uint32_t symbol, double bid_price,
                                  double bid_qty, double ask_price, double ask_qty)
{
  std::pmr::monotonic_buffer_resource pool(512);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.emplace_back(Price::fromDouble(bid_price), Quantity::fromDouble(bid_qty));
  asks.emplace_back(Price::fromDouble(ask_price), Quantity::fromDouble(ask_qty));
  static_cast<FloxExecutorImpl*>(h)->executor.onBookUpdate(symbol, bids, asks);
}

void flox_executor_on_book_snapshot(FloxExecutorHandle h, uint32_t symbol,
                                    const double* bid_prices, const double* bid_qtys,
                                    uint32_t n_bids, const double* ask_prices,
                                    const double* ask_qtys, uint32_t n_asks)
{
  std::pmr::monotonic_buffer_resource pool(1024);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.reserve(n_bids);
  asks.reserve(n_asks);
  for (uint32_t i = 0; i < n_bids; ++i)
  {
    bids.emplace_back(Price::fromDouble(bid_prices[i]), Quantity::fromDouble(bid_qtys[i]));
  }
  for (uint32_t i = 0; i < n_asks; ++i)
  {
    asks.emplace_back(Price::fromDouble(ask_prices[i]), Quantity::fromDouble(ask_qtys[i]));
  }
  static_cast<FloxExecutorImpl*>(h)->executor.onBookUpdate(symbol, bids, asks);
}

struct FloxBacktestResultImpl
{
  BacktestConfig config;
  std::unique_ptr<BacktestResult> result;
};

FloxBacktestResultHandle flox_backtest_result_create(double initial_capital, double fee_rate,
                                                     uint8_t use_percentage_fee,
                                                     double fixed_fee_per_trade,
                                                     double risk_free_rate,
                                                     double annualization_factor)
{
  auto* impl = new FloxBacktestResultImpl();
  impl->config.initialCapital = initial_capital;
  impl->config.feeRate = fee_rate;
  impl->config.usePercentageFee = use_percentage_fee != 0;
  impl->config.fixedFeePerTrade = fixed_fee_per_trade;
  impl->config.riskFreeRate = risk_free_rate;
  impl->config.metricsAnnualizationFactor =
      (annualization_factor > 0.0) ? annualization_factor : 252.0;
  impl->result = std::make_unique<BacktestResult>(impl->config);
  return impl;
}

void flox_backtest_result_destroy(FloxBacktestResultHandle h)
{
  delete static_cast<FloxBacktestResultImpl*>(h);
}

void flox_backtest_result_record_fill(FloxBacktestResultHandle h, uint64_t order_id,
                                      uint32_t symbol, uint8_t side, double price,
                                      double quantity, int64_t timestamp_ns)
{
  Fill fill{};
  fill.orderId = order_id;
  fill.symbol = symbol;
  fill.side = (side == 0) ? Side::BUY : Side::SELL;
  fill.price = Price::fromDouble(price);
  fill.quantity = Quantity::fromDouble(quantity);
  fill.timestampNs = static_cast<UnixNanos>(timestamp_ns);
  static_cast<FloxBacktestResultImpl*>(h)->result->recordFill(fill);
}

void flox_backtest_result_ingest_executor(FloxBacktestResultHandle h, FloxExecutorHandle eh)
{
  auto* impl = static_cast<FloxBacktestResultImpl*>(h);
  for (const auto& fill : static_cast<FloxExecutorImpl*>(eh)->executor.fills())
  {
    impl->result->recordFill(fill);
  }
}

void flox_backtest_result_stats(FloxBacktestResultHandle h, FloxBacktestStats* out)
{
  if (!out)
  {
    return;
  }
  auto stats = static_cast<FloxBacktestResultImpl*>(h)->result->computeStats();
  out->totalTrades = stats.totalTrades;
  out->winningTrades = stats.winningTrades;
  out->losingTrades = stats.losingTrades;
  out->maxConsecutiveWins = stats.maxConsecutiveWins;
  out->maxConsecutiveLosses = stats.maxConsecutiveLosses;
  out->initialCapital = stats.initialCapital;
  out->finalCapital = stats.finalCapital;
  out->totalPnl = stats.totalPnl;
  out->totalFees = stats.totalFees;
  out->netPnl = stats.netPnl;
  out->grossProfit = stats.grossProfit;
  out->grossLoss = stats.grossLoss;
  out->maxDrawdown = stats.maxDrawdown;
  out->maxDrawdownPct = stats.maxDrawdownPct;
  out->winRate = stats.winRate;
  out->profitFactor = stats.profitFactor;
  out->avgWin = stats.avgWin;
  out->avgLoss = stats.avgLoss;
  out->avgWinLossRatio = stats.avgWinLossRatio;
  out->avgTradeDurationNs = stats.avgTradeDurationNs;
  out->medianTradeDurationNs = stats.medianTradeDurationNs;
  out->maxTradeDurationNs = stats.maxTradeDurationNs;
  out->sharpeRatio = stats.sharpeRatio;
  out->sortinoRatio = stats.sortinoRatio;
  out->calmarRatio = stats.calmarRatio;
  out->timeWeightedReturn = stats.timeWeightedReturn;
  out->returnPct = stats.returnPct;
  out->startTimeNs = static_cast<int64_t>(stats.startTimeNs);
  out->endTimeNs = static_cast<int64_t>(stats.endTimeNs);
}

uint32_t flox_backtest_result_equity_curve(FloxBacktestResultHandle h, FloxEquityPoint* out,
                                           uint32_t max_points)
{
  const auto& curve = static_cast<FloxBacktestResultImpl*>(h)->result->equityCurve();
  const uint32_t total = static_cast<uint32_t>(curve.size());
  if (!out)
  {
    return total;
  }
  const uint32_t n = (total < max_points) ? total : max_points;
  for (uint32_t i = 0; i < n; ++i)
  {
    out[i].timestamp_ns = static_cast<int64_t>(curve[i].timestampNs);
    out[i].equity = curve[i].equity;
    out[i].drawdown_pct = curve[i].drawdownPct;
  }
  return n;
}

uint8_t flox_backtest_result_write_equity_curve_csv(FloxBacktestResultHandle h, const char* path)
{
  if (!path)
  {
    return 0;
  }
  return static_cast<FloxBacktestResultImpl*>(h)->result->writeEquityCurveCsv(path) ? 1 : 0;
}

// ============================================================
// Segment operations (full API)
// ============================================================

static replay::CompressionType toCompression(uint8_t c)
{
  return c == 1 ? replay::CompressionType::LZ4 : replay::CompressionType::None;
}

FloxMergeResult flox_segment_merge_full(const char* input_paths, size_t num_paths,
                                        const char* output_dir, const char* output_name,
                                        uint8_t sort)
{
  std::vector<std::filesystem::path> paths;
  const char* p = input_paths;
  for (size_t i = 0; i < num_paths; ++i)
  {
    paths.emplace_back(p);
    p += std::strlen(p) + 1;
  }
  replay::MergeConfig cfg;
  cfg.output_dir = output_dir;
  cfg.output_name = output_name ? output_name : "merged";
  cfg.sort_by_timestamp = sort != 0;
  auto r = replay::SegmentOps::merge(paths, cfg);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.segments_merged, r.events_written,
          r.bytes_written};
}

FloxMergeResult flox_segment_merge_dir(const char* input_dir, const char* output_dir)
{
  auto r = replay::quickMerge(input_dir, output_dir);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.segments_merged, r.events_written,
          r.bytes_written};
}

FloxSplitResult flox_segment_split(const char* input_path, const char* output_dir, uint8_t mode,
                                   int64_t time_interval_ns, uint64_t events_per_file)
{
  replay::SplitConfig cfg;
  cfg.output_dir = output_dir;
  cfg.mode = static_cast<replay::SplitMode>(mode);
  cfg.time_interval_ns = time_interval_ns;
  cfg.events_per_file = events_per_file;
  auto r = replay::SegmentOps::split(input_path, cfg);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.segments_created, r.events_written};
}

FloxExportResult flox_segment_export(const char* input_path, const char* output_path,
                                     uint8_t format, int64_t from_ns, int64_t to_ns,
                                     const uint32_t* symbols, uint32_t num_symbols)
{
  replay::ExportConfig cfg;
  cfg.output_path = output_path;
  cfg.format = static_cast<replay::ExportFormat>(format);
  if (from_ns > 0)
  {
    cfg.from_ts = from_ns;
  }
  if (to_ns > 0)
  {
    cfg.to_ts = to_ns;
  }
  if (symbols && num_symbols > 0)
  {
    cfg.symbols.insert(symbols, symbols + num_symbols);
  }
  auto r = replay::SegmentOps::exportData(input_path, cfg);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.events_exported, r.bytes_written};
}

uint8_t flox_segment_recompress(const char* input_path, const char* output_path,
                                uint8_t compression)
{
  return replay::SegmentOps::recompress(input_path, output_path, toCompression(compression)) ? 1
                                                                                             : 0;
}

uint64_t flox_segment_extract_symbols(const char* input_path, const char* output_path,
                                      const uint32_t* symbols, uint32_t num_symbols)
{
  std::set<uint32_t> symSet(symbols, symbols + num_symbols);
  replay::WriterConfig wc;
  auto out = std::filesystem::path(output_path);
  wc.output_dir = out.parent_path();
  wc.output_filename = out.filename().string();
  return replay::SegmentOps::extractSymbols(input_path, output_path, symSet, wc);
}

uint64_t flox_segment_extract_time_range(const char* input_path, const char* output_path,
                                         int64_t from_ns, int64_t to_ns)
{
  replay::WriterConfig wc;
  auto out = std::filesystem::path(output_path);
  wc.output_dir = out.parent_path();
  wc.output_filename = out.filename().string();
  return replay::SegmentOps::extractTimeRange(input_path, output_path, from_ns, to_ns, wc);
}

// ============================================================
// Validation (full API)
// ============================================================

FloxSegmentValidation flox_segment_validate_full(const char* path, uint8_t verify_crc,
                                                 uint8_t verify_timestamps)
{
  replay::ValidatorConfig cfg;
  cfg.verify_crc = verify_crc != 0;
  cfg.verify_timestamps = verify_timestamps != 0;
  replay::SegmentValidator validator(cfg);
  auto r = validator.validate(path);
  return {r.valid ? (uint8_t)1 : (uint8_t)0,
          r.header_valid ? (uint8_t)1 : (uint8_t)0,
          r.reported_event_count,
          r.actual_event_count,
          r.has_index ? (uint8_t)1 : (uint8_t)0,
          r.index_valid ? (uint8_t)1 : (uint8_t)0,
          r.trades_found,
          r.book_updates_found,
          r.crc_errors,
          r.timestamp_anomalies};
}

FloxDatasetValidation flox_dataset_validate(const char* data_dir)
{
  replay::DatasetValidator validator;
  auto r = validator.validate(data_dir);
  return {r.valid ? (uint8_t)1 : (uint8_t)0, r.total_segments, r.valid_segments,
          r.corrupted_segments, r.total_events, r.total_bytes,
          r.first_timestamp, r.last_timestamp};
}

// ============================================================
// DataReader (full API)
// ============================================================

FloxDataReaderHandle flox_data_reader_create_filtered(const char* data_dir, int64_t from_ns,
                                                      int64_t to_ns, const uint32_t* symbols,
                                                      uint32_t num_symbols)
{
  replay::ReaderConfig cfg;
  cfg.data_dir = data_dir;
  if (from_ns > 0)
  {
    cfg.from_ns = from_ns;
  }
  if (to_ns > 0)
  {
    cfg.to_ns = to_ns;
  }
  if (symbols && num_symbols > 0)
  {
    cfg.symbols.insert(symbols, symbols + num_symbols);
  }
  return new replay::BinaryLogReader(cfg);
}

FloxDatasetSummary flox_data_reader_summary(FloxDataReaderHandle h)
{
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  auto s = reader->summary();
  return {s.first_event_ns, s.last_event_ns, s.total_events, s.segment_count, s.total_bytes,
          s.durationSeconds()};
}

FloxReaderStats flox_data_reader_stats(FloxDataReaderHandle h)
{
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  auto s = reader->stats();
  return {s.files_read, s.events_read, s.trades_read, s.book_updates_read, s.bytes_read,
          s.crc_errors};
}

uint64_t flox_data_reader_read_trades(FloxDataReaderHandle h, FloxTradeRecord* trades_out,
                                      uint64_t max_trades)
{
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type == replay::EventType::Trade)
        {
          if (trades_out && count < max_trades)
          {
            trades_out[count] = {ev.trade.exchange_ts_ns, ev.trade.recv_ts_ns, ev.trade.price_raw,
                                 ev.trade.qty_raw, ev.trade.trade_id, ev.trade.symbol_id,
                                 ev.trade.side};
          }
          ++count;
        }
        return !trades_out || count < max_trades;
      });
  return count;
}

// ============================================================
// DataWriter (extras)
// ============================================================

FloxWriterStats flox_data_writer_stats(FloxDataWriterHandle h)
{
  auto* w = static_cast<replay::BinaryLogWriter*>(h);
  auto s = w->stats();
  return {s.bytes_written, s.events_written, s.segments_created, s.trades_written};
}

// ============================================================
// DataRecorder
// ============================================================

FloxDataRecorderHandle flox_data_recorder_create(const char* output_dir,
                                                 const char* exchange_name,
                                                 uint64_t max_segment_mb)
{
  MarketDataRecorderConfig cfg;
  cfg.output_dir = output_dir;
  cfg.exchange_name = exchange_name;
  cfg.max_segment_bytes = max_segment_mb * 1024 * 1024;
  return new MarketDataRecorder(cfg);
}

void flox_data_recorder_destroy(FloxDataRecorderHandle h)
{
  delete static_cast<MarketDataRecorder*>(h);
}

void flox_data_recorder_add_symbol(FloxDataRecorderHandle h, uint32_t symbol_id, const char* name,
                                   const char* base, const char* quote, int8_t price_precision,
                                   int8_t qty_precision)
{
  static_cast<MarketDataRecorder*>(h)->addSymbol(symbol_id, name, base ? base : "",
                                                 quote ? quote : "", price_precision,
                                                 qty_precision);
}

void flox_data_recorder_start(FloxDataRecorderHandle h)
{
  static_cast<MarketDataRecorder*>(h)->start();
}

void flox_data_recorder_stop(FloxDataRecorderHandle h)
{
  static_cast<MarketDataRecorder*>(h)->stop();
}

void flox_data_recorder_flush(FloxDataRecorderHandle h)
{
  static_cast<MarketDataRecorder*>(h)->flush();
}

uint8_t flox_data_recorder_is_recording(FloxDataRecorderHandle h)
{
  return static_cast<MarketDataRecorder*>(h)->isRecording() ? 1 : 0;
}

// ============================================================
// Partitioner
// ============================================================

FloxPartitionerHandle flox_partitioner_create(const char* data_dir)
{
  return new replay::Partitioner(std::filesystem::path(data_dir));
}

void flox_partitioner_destroy(FloxPartitionerHandle h)
{
  delete static_cast<replay::Partitioner*>(h);
}

static uint32_t copyPartitions(const std::vector<replay::Partition>& parts,
                               FloxPartition* out, uint32_t max)
{
  uint32_t n = static_cast<uint32_t>(parts.size());
  if (!out)
  {
    return n;
  }
  uint32_t count = std::min(n, max);
  for (uint32_t i = 0; i < count; ++i)
  {
    out[i] = {parts[i].partition_id, parts[i].from_ns, parts[i].to_ns,
              parts[i].warmup_from_ns, parts[i].estimated_events, parts[i].estimated_bytes};
  }
  return count;
}

uint32_t flox_partitioner_by_time(FloxPartitionerHandle h, uint32_t num_partitions,
                                  int64_t warmup_ns, FloxPartition* out, uint32_t max)
{
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByTime(num_partitions, warmup_ns), out, max);
}

uint32_t flox_partitioner_by_duration(FloxPartitionerHandle h, int64_t duration_ns,
                                      int64_t warmup_ns, FloxPartition* out, uint32_t max)
{
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByDuration(duration_ns, warmup_ns), out, max);
}

uint32_t flox_partitioner_by_calendar(FloxPartitionerHandle h, uint8_t unit, int64_t warmup_ns,
                                      FloxPartition* out, uint32_t max)
{
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByCalendar(
          static_cast<replay::Partitioner::CalendarUnit>(unit), warmup_ns),
      out, max);
}

uint32_t flox_partitioner_by_symbol(FloxPartitionerHandle h, uint32_t num_partitions,
                                    FloxPartition* out, uint32_t max)
{
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionBySymbol(num_partitions), out, max);
}

uint32_t flox_partitioner_per_symbol(FloxPartitionerHandle h, FloxPartition* out, uint32_t max)
{
  return copyPartitions(static_cast<replay::Partitioner*>(h)->partitionPerSymbol(), out, max);
}

uint32_t flox_partitioner_by_event_count(FloxPartitionerHandle h, uint32_t num_partitions,
                                         FloxPartition* out, uint32_t max)
{
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByEventCount(num_partitions), out, max);
}

// ============================================================
// Pointer-out wrappers for struct-returning functions.
// ============================================================

void flox_data_reader_summary_p(FloxDataReaderHandle h, void* out)
{
  auto s = flox_data_reader_summary(h);
  memcpy(out, &s, sizeof(s));
}

void flox_data_reader_stats_p(FloxDataReaderHandle h, void* out)
{
  auto s = flox_data_reader_stats(h);
  memcpy(out, &s, sizeof(s));
}

void flox_data_writer_stats_p(FloxDataWriterHandle h, void* out)
{
  auto s = flox_data_writer_stats(h);
  memcpy(out, &s, sizeof(s));
}

void flox_segment_merge_full_p(const char* input_paths, size_t num_paths,
                               const char* output_dir, const char* output_name,
                               uint8_t sort, void* out)
{
  auto s = flox_segment_merge_full(input_paths, num_paths, output_dir, output_name, sort);
  memcpy(out, &s, sizeof(s));
}

void flox_segment_merge_dir_p(const char* input_dir, const char* output_dir, void* out)
{
  auto s = flox_segment_merge_dir(input_dir, output_dir);
  memcpy(out, &s, sizeof(s));
}

void flox_segment_split_p(const char* input_path, const char* output_dir, uint8_t mode,
                          int64_t time_interval_ns, uint64_t events_per_file, void* out)
{
  auto s = flox_segment_split(input_path, output_dir, mode, time_interval_ns, events_per_file);
  memcpy(out, &s, sizeof(s));
}

void flox_segment_export_p(const char* input_path, const char* output_path, uint8_t format,
                           int64_t from_ns, int64_t to_ns,
                           const uint32_t* symbols, uint32_t num_symbols, void* out)
{
  auto s = flox_segment_export(input_path, output_path, format, from_ns, to_ns,
                               symbols, num_symbols);
  memcpy(out, &s, sizeof(s));
}

void flox_segment_validate_full_p(const char* path, uint8_t verify_crc,
                                  uint8_t verify_timestamps, void* out)
{
  auto s = flox_segment_validate_full(path, verify_crc, verify_timestamps);
  memcpy(out, &s, sizeof(s));
}

void flox_dataset_validate_p(const char* data_dir, void* out)
{
  auto s = flox_dataset_validate(data_dir);
  memcpy(out, &s, sizeof(s));
}

// ============================================================
// Shared internals: RunnerSignalHandler, FloxRunnerImpl, FloxLiveEngineImpl
// ============================================================

namespace capi_impl
{

class RunnerSignalHandler : public ISignalHandler
{
 public:
  RunnerSignalHandler(FloxOnSignalCallback cb, void* ud) : _cb(cb), _ud(ud) {}

  void onSignal(const Signal& sig) override
  {
    if (!_cb)
    {
      return;
    }
    FloxSignal fs{};
    fs.order_id = sig.orderId;
    fs.symbol = sig.symbol;
    fs.side = (sig.side == Side::BUY) ? 0 : 1;
    fs.price = sig.price.toDouble();
    fs.quantity = sig.quantity.toDouble();
    fs.trigger_price = sig.triggerPrice.toDouble();
    fs.trailing_offset = sig.trailingOffset.toDouble();
    fs.trailing_bps = sig.trailingCallbackRate;
    fs.new_price = sig.newPrice.toDouble();
    fs.new_quantity = sig.newQuantity.toDouble();

    switch (sig.type)
    {
      case SignalType::Market:
        fs.order_type = 0;
        break;
      case SignalType::Limit:
        fs.order_type = 1;
        break;
      case SignalType::StopMarket:
        fs.order_type = 2;
        break;
      case SignalType::StopLimit:
        fs.order_type = 3;
        break;
      case SignalType::TakeProfitMarket:
        fs.order_type = 4;
        break;
      case SignalType::TakeProfitLimit:
        fs.order_type = 5;
        break;
      case SignalType::TrailingStop:
        fs.order_type = 6;
        break;
      case SignalType::Cancel:
        fs.order_type = 7;
        break;
      case SignalType::CancelAll:
        fs.order_type = 8;
        break;
      case SignalType::Modify:
        fs.order_type = 9;
        break;
      default:
        fs.order_type = 0;
        break;
    }
    _cb(_ud, &fs);
  }

 private:
  FloxOnSignalCallback _cb;
  void* _ud;
};

struct FloxRunnerImpl
{
  SymbolRegistry* registry;
  RunnerSignalHandler handler;
  std::vector<BridgeStrategy*> strategies;
  std::pmr::unsynchronized_pool_resource pool;

  FloxRunnerImpl(SymbolRegistry* reg, FloxOnSignalCallback cb, void* ud)
      : registry(reg), handler(cb, ud)
  {
  }

  void addStrategy(BridgeStrategy* s)
  {
    s->setSignalHandler(&handler);
    strategies.push_back(s);
  }

  void start()
  {
    for (auto* s : strategies)
    {
      s->start();
    }
  }

  void stop()
  {
    for (auto* s : strategies)
    {
      s->stop();
    }
  }

  void onTrade(uint32_t symbol, double price, double qty, bool is_buy, int64_t ts_ns)
  {
    Trade trade{};
    trade.symbol = symbol;
    trade.price = Price::fromDouble(price);
    trade.quantity = Quantity::fromDouble(qty);
    trade.isBuy = is_buy;
    trade.exchangeTsNs = UnixNanos(ts_ns);

    TradeEvent ev{};
    ev.trade = trade;

    for (auto* s : strategies)
    {
      s->onTrade(ev);
    }
  }

  void onBookSnapshot(uint32_t symbol,
                      const double* bid_prices, const double* bid_qtys, uint32_t n_bids,
                      const double* ask_prices, const double* ask_qtys, uint32_t n_asks,
                      int64_t ts_ns)
  {
    BookUpdateEvent ev(&pool);
    ev.update.symbol = symbol;
    ev.update.type = BookUpdateType::SNAPSHOT;
    ev.update.exchangeTsNs = UnixNanos(ts_ns);

    for (uint32_t i = 0; i < n_bids; ++i)
    {
      ev.update.bids.push_back({Price::fromDouble(bid_prices[i]),
                                Quantity::fromDouble(bid_qtys[i])});
    }
    for (uint32_t i = 0; i < n_asks; ++i)
    {
      ev.update.asks.push_back({Price::fromDouble(ask_prices[i]),
                                Quantity::fromDouble(ask_qtys[i])});
    }

    for (auto* s : strategies)
    {
      s->onBookUpdate(ev);
    }
  }
};

static FloxRunnerImpl* toRunner(FloxRunnerHandle h)
{
  return static_cast<FloxRunnerImpl*>(h);
}

// ============================================================
// FloxLiveEngineImpl — real Disruptor-based live engine
// ============================================================

struct FloxLiveEngineImpl
{
  SymbolRegistry* registry;
  std::unique_ptr<TradeBus> tradeBus;
  std::unique_ptr<BookUpdateBus> bookBus;
  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> bookPool;

  // Per-strategy signal handlers (owned here, outlive strategies)
  std::vector<std::unique_ptr<RunnerSignalHandler>> handlers;
  std::vector<BridgeStrategy*> strategies;

  explicit FloxLiveEngineImpl(SymbolRegistry* reg)
      : registry(reg), tradeBus(std::make_unique<TradeBus>()), bookBus(std::make_unique<BookUpdateBus>())
  {
  }

  void addStrategy(BridgeStrategy* s, FloxOnSignalCallback cb, void* ud)
  {
    auto h = std::make_unique<RunnerSignalHandler>(cb, ud);
    s->setSignalHandler(h.get());
    handlers.push_back(std::move(h));
    tradeBus->subscribe(s);
    bookBus->subscribe(s);
    strategies.push_back(s);
  }

  void start()
  {
    tradeBus->start();
    bookBus->start();
    for (auto* s : strategies)
    {
      s->start();
    }
  }

  void stop()
  {
    for (auto* s : strategies)
    {
      s->stop();
    }
    tradeBus->stop();
    bookBus->stop();
  }

  void publishTrade(uint32_t symbol, double price, double qty, bool is_buy, int64_t ts_ns)
  {
    TradeEvent ev{};
    ev.trade.symbol = symbol;
    ev.trade.price = Price::fromDouble(price);
    ev.trade.quantity = Quantity::fromDouble(qty);
    ev.trade.isBuy = is_buy;
    ev.trade.exchangeTsNs = UnixNanos(ts_ns);
    tradeBus->publish(ev);
  }

  void publishBookSnapshot(uint32_t symbol,
                           const double* bid_prices, const double* bid_qtys, uint32_t n_bids,
                           const double* ask_prices, const double* ask_qtys, uint32_t n_asks,
                           int64_t ts_ns)
  {
    auto evOpt = bookPool.acquire();
    if (!evOpt)
    {
      return;
    }
    auto& ev = *evOpt;
    ev->update.symbol = symbol;
    ev->update.type = BookUpdateType::SNAPSHOT;
    ev->update.exchangeTsNs = UnixNanos(ts_ns);
    ev->update.bids.clear();
    ev->update.asks.clear();
    for (uint32_t i = 0; i < n_bids; ++i)
    {
      ev->update.bids.push_back({Price::fromDouble(bid_prices[i]),
                                 Quantity::fromDouble(bid_qtys[i])});
    }
    for (uint32_t i = 0; i < n_asks; ++i)
    {
      ev->update.asks.push_back({Price::fromDouble(ask_prices[i]),
                                 Quantity::fromDouble(ask_qtys[i])});
    }
    bookBus->publish(std::move(evOpt.value()));
  }
};

static FloxLiveEngineImpl* toLiveEngine(FloxLiveEngineHandle h)
{
  return static_cast<FloxLiveEngineImpl*>(h);
}

// ──────────────────────────────────────────────────────────────
// OhlcvBacktestReader — synthesises trade events from OHLCV bars
// ──────────────────────────────────────────────────────────────

class OhlcvBacktestReader : public replay::IMultiSegmentReader
{
 public:
  struct Bar
  {
    int64_t ts_ns;
    int64_t price_raw;
    uint32_t symbol_id;
  };

  explicit OhlcvBacktestReader(std::vector<Bar> bars) : _bars(std::move(bars)) {}

  uint64_t forEach(EventCallback cb) override
  {
    uint64_t n = 0;
    for (const auto& b : _bars)
    {
      if (!cb(make(b)))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  uint64_t forEachFrom(int64_t start_ns, EventCallback cb) override
  {
    uint64_t n = 0;
    for (const auto& b : _bars)
    {
      if (b.ts_ns < start_ns)
      {
        continue;
      }
      if (!cb(make(b)))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  const std::vector<replay::SegmentInfo>& segments() const override { return _segs; }
  uint64_t totalEvents() const override { return _bars.size(); }

 private:
  static replay::ReplayEvent make(const Bar& b)
  {
    replay::ReplayEvent ev{};
    ev.type = replay::EventType::Trade;
    ev.timestamp_ns = b.ts_ns;
    ev.trade.exchange_ts_ns = b.ts_ns;
    ev.trade.price_raw = b.price_raw;
    ev.trade.qty_raw = Quantity::fromDouble(1.0).raw();
    ev.trade.symbol_id = b.symbol_id;
    ev.trade.side = 1;
    return ev;
  }

  std::vector<Bar> _bars;
  std::vector<replay::SegmentInfo> _segs;
};

// ──────────────────────────────────────────────────────────────
// FloxBacktestRunnerImpl
// ──────────────────────────────────────────────────────────────

struct FloxBacktestRunnerImpl
{
  SymbolRegistry* registry;
  std::unique_ptr<BacktestRunner> runner;

  explicit FloxBacktestRunnerImpl(SymbolRegistry* reg, double feeRate, double initialCapital)
      : registry(reg)
  {
    BacktestConfig cfg{};
    cfg.feeRate = feeRate;
    cfg.initialCapital = initialCapital;
    cfg.usePercentageFee = true;
    runner = std::make_unique<BacktestRunner>(cfg);
  }

  void setStrategy(BridgeStrategy* bridge)
  {
    runner->setStrategy(bridge);
  }

  static int64_t normalizeTs(int64_t t)
  {
    if (t < static_cast<int64_t>(1e12))
    {
      return t * 1'000'000'000LL;
    }
    if (t < static_cast<int64_t>(1e15))
    {
      return t * 1'000'000LL;
    }
    if (t < static_cast<int64_t>(1e18))
    {
      return t * 1'000LL;
    }
    return t;
  }

  uint32_t resolveSymbol(const char* symbol) const
  {
    if (!symbol || symbol[0] == '\0')
    {
      auto all = registry->getAllSymbols();
      return all.empty() ? 0 : all.front().id;
    }
    auto all = registry->getAllSymbols();
    for (const auto& info : all)
    {
      if (info.symbol == symbol)
      {
        return info.id;
      }
    }
    return 0;
  }

  static void fillStats(const BacktestStats& s, FloxBacktestStats* out)
  {
    out->totalTrades = s.totalTrades;
    out->winningTrades = s.winningTrades;
    out->losingTrades = s.losingTrades;
    out->maxConsecutiveWins = s.maxConsecutiveWins;
    out->maxConsecutiveLosses = s.maxConsecutiveLosses;
    out->initialCapital = s.initialCapital;
    out->finalCapital = s.finalCapital;
    out->totalPnl = s.totalPnl;
    out->totalFees = s.totalFees;
    out->netPnl = s.netPnl;
    out->grossProfit = s.grossProfit;
    out->grossLoss = s.grossLoss;
    out->maxDrawdown = s.maxDrawdown;
    out->maxDrawdownPct = s.maxDrawdownPct;
    out->winRate = s.winRate;
    out->profitFactor = s.profitFactor;
    out->avgWin = s.avgWin;
    out->avgLoss = s.avgLoss;
    out->avgWinLossRatio = s.avgWinLossRatio;
    out->sharpeRatio = s.sharpeRatio;
    out->sortinoRatio = s.sortinoRatio;
    out->calmarRatio = s.calmarRatio;
    out->returnPct = s.returnPct;
  }

  int runBars(std::vector<OhlcvBacktestReader::Bar> bars, FloxBacktestStats* out)
  {
    OhlcvBacktestReader reader(std::move(bars));
    BacktestResult result = runner->run(reader);
    if (out)
    {
      fillStats(result.computeStats(), out);
    }
    return 1;
  }

  int runCsv(const char* path, const char* symbol, FloxBacktestStats* out)
  {
    uint32_t id = resolveSymbol(symbol);
    std::ifstream f(path);
    if (!f.is_open())
    {
      return 0;
    }

    std::vector<OhlcvBacktestReader::Bar> bars;
    std::string line;
    std::getline(f, line);  // skip header
    while (std::getline(f, line))
    {
      if (line.empty())
      {
        continue;
      }
      std::istringstream ss(line);
      std::string tok;
      std::getline(ss, tok, ',');
      int64_t ts = normalizeTs(std::stoll(tok));
      std::getline(ss, tok, ',');  // open
      std::getline(ss, tok, ',');  // high
      std::getline(ss, tok, ',');  // low
      std::getline(ss, tok, ',');
      double c = std::stod(tok);
      bars.push_back({ts, Price::fromDouble(c).raw(), id});
    }
    return runBars(std::move(bars), out);
  }

  int runOhlcv(const int64_t* ts, const double* close, uint32_t n,
               const char* symbol, FloxBacktestStats* out)
  {
    uint32_t id = resolveSymbol(symbol);
    std::vector<OhlcvBacktestReader::Bar> bars;
    bars.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      bars.push_back({normalizeTs(ts[i]), Price::fromDouble(close[i]).raw(), id});
    }
    return runBars(std::move(bars), out);
  }
};

static FloxBacktestRunnerImpl* toBacktestRunner(FloxBacktestRunnerHandle h)
{
  return static_cast<FloxBacktestRunnerImpl*>(h);
}

}  // namespace capi_impl

using namespace capi_impl;

FloxRunnerHandle flox_runner_create(FloxRegistryHandle registry,
                                    FloxOnSignalCallback on_signal,
                                    void* user_data)
{
  return static_cast<FloxRunnerHandle>(
      new FloxRunnerImpl(toRegistry(registry), on_signal, user_data));
}

void flox_runner_destroy(FloxRunnerHandle runner)
{
  delete toRunner(runner);
}

void flox_runner_add_strategy(FloxRunnerHandle runner, FloxStrategyHandle strategy)
{
  toRunner(runner)->addStrategy(toStrategy(strategy));
}

void flox_runner_start(FloxRunnerHandle runner)
{
  toRunner(runner)->start();
}

void flox_runner_stop(FloxRunnerHandle runner)
{
  toRunner(runner)->stop();
}

void flox_runner_on_trade(FloxRunnerHandle runner, uint32_t symbol,
                          double price, double qty, uint8_t is_buy,
                          int64_t exchange_ts_ns)
{
  toRunner(runner)->onTrade(symbol, price, qty, is_buy != 0, exchange_ts_ns);
}

void flox_runner_on_book_snapshot(FloxRunnerHandle runner, uint32_t symbol,
                                  const double* bid_prices, const double* bid_qtys,
                                  uint32_t n_bids,
                                  const double* ask_prices, const double* ask_qtys,
                                  uint32_t n_asks,
                                  int64_t exchange_ts_ns)
{
  toRunner(runner)->onBookSnapshot(symbol,
                                   bid_prices, bid_qtys, n_bids,
                                   ask_prices, ask_qtys, n_asks,
                                   exchange_ts_ns);
}

// ============================================================
// FloxLiveEngine C API
// ============================================================

FloxLiveEngineHandle flox_live_engine_create(FloxRegistryHandle registry)
{
  return static_cast<FloxLiveEngineHandle>(new FloxLiveEngineImpl(toRegistry(registry)));
}

void flox_live_engine_destroy(FloxLiveEngineHandle engine)
{
  delete toLiveEngine(engine);
}

void flox_live_engine_add_strategy(FloxLiveEngineHandle engine,
                                   FloxStrategyHandle strategy,
                                   FloxOnSignalCallback on_signal,
                                   void* user_data)
{
  toLiveEngine(engine)->addStrategy(toStrategy(strategy), on_signal, user_data);
}

void flox_live_engine_start(FloxLiveEngineHandle engine)
{
  toLiveEngine(engine)->start();
}

void flox_live_engine_stop(FloxLiveEngineHandle engine)
{
  toLiveEngine(engine)->stop();
}

void flox_live_engine_publish_trade(FloxLiveEngineHandle engine,
                                    uint32_t symbol,
                                    double price, double qty, uint8_t is_buy,
                                    int64_t exchange_ts_ns)
{
  toLiveEngine(engine)->publishTrade(symbol, price, qty, is_buy != 0, exchange_ts_ns);
}

void flox_live_engine_publish_book_snapshot(FloxLiveEngineHandle engine,
                                            uint32_t symbol,
                                            const double* bid_prices,
                                            const double* bid_qtys,
                                            uint32_t n_bids,
                                            const double* ask_prices,
                                            const double* ask_qtys,
                                            uint32_t n_asks,
                                            int64_t exchange_ts_ns)
{
  toLiveEngine(engine)->publishBookSnapshot(symbol,
                                            bid_prices, bid_qtys, n_bids,
                                            ask_prices, ask_qtys, n_asks,
                                            exchange_ts_ns);
}

// ============================================================
// FloxBacktestRunner C API
// ============================================================

FloxBacktestRunnerHandle flox_backtest_runner_create(FloxRegistryHandle registry,
                                                     double fee_rate,
                                                     double initial_capital)
{
  return static_cast<FloxBacktestRunnerHandle>(
      new FloxBacktestRunnerImpl(toRegistry(registry), fee_rate, initial_capital));
}

void flox_backtest_runner_destroy(FloxBacktestRunnerHandle h)
{
  delete toBacktestRunner(h);
}

void flox_backtest_runner_set_strategy(FloxBacktestRunnerHandle h,
                                       FloxStrategyHandle strategy)
{
  toBacktestRunner(h)->setStrategy(toStrategy(strategy));
}

int flox_backtest_runner_run_csv(FloxBacktestRunnerHandle h,
                                 const char* path,
                                 const char* symbol,
                                 FloxBacktestStats* out)
{
  return toBacktestRunner(h)->runCsv(path, symbol, out);
}

int flox_backtest_runner_run_ohlcv(FloxBacktestRunnerHandle h,
                                   const int64_t* ts,
                                   const double* close,
                                   uint32_t n,
                                   const char* symbol,
                                   FloxBacktestStats* out)
{
  return toBacktestRunner(h)->runOhlcv(ts, close, n, symbol, out);
}
