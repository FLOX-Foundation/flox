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
#include "flox/log/abstract_logger.h"
#include "flox/log/log_stream.h"

#include "flox/aggregator/bar.h"
#include "flox/aggregator/policies/tick_bar_policy.h"
#include "flox/aggregator/policies/time_bar_policy.h"
#include "flox/aggregator/policies/volume_bar_policy.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/grid_search.h"
#include "flox/backtest/latency_model.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/backtest/walk_forward.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"
#include "flox/execution/algos.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/replay/aggregator.h"
#include "flox/replay/aggregators/bin_count.h"
#include "flox/replay/aggregators/event_type_stats.h"
#include "flox/replay/aggregators/ohlc_bin.h"
#include "flox/replay/aggregators/peak.h"
#include "flox/replay/aggregators/quantile.h"
#include "flox/replay/aggregators/volume_bin.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/delta_book.h"
#include "flox/replay/ohlcv_replay_source.h"
#include "flox/replay/tape_diff.h"
#include "flox/report/heatmap_html.h"
#include "flox/risk/portfolio_risk.h"
#include "flox/run/trace_reader.h"
#include "flox/run/trace_recorder.h"
#include "flox/stats/whites_reality_check.h"

#include "flox/indicator/adf.h"
#include "flox/indicator/adx.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/autocorrelation.h"
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
#include "flox/indicator/streaming_graph.h"
#include "flox/indicator/vwap.h"
#include "flox/target/future_ctc_volatility.h"
#include "flox/target/future_linear_slope.h"
#include "flox/target/future_return.h"

#include "flox/aggregator/bus/bar_bus.h"
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
#include "flox/execution/abstract_execution_listener.h"
#include "flox/execution/abstract_executor.h"
#include "flox/execution/exchange_capabilities.h"
#include "flox/execution/order.h"
#include "flox/execution/order_journey_tracer.h"
#include "flox/execution/order_tracker.h"
#include "flox/position/position_group.h"
#include "flox/position/position_tracker.h"
#include "flox/replay/binary_log_recorder_hook.h"
#include "flox/replay/merged_tape_reader.h"
#include "flox/replay/ops/partitioner.h"
#include "flox/replay/ops/segment_ops.h"
#include "flox/replay/ops/validator.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"
#include "flox/strategy/abstract_signal_handler.h"
#include "flox/testing/bar_dispatch_recorder.h"
#include "flox/util/memory/pool.h"

#include <random>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
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

FloxStrategyHandle flox_strategy_create_p(uint32_t id, const uint32_t* symbols,
                                          uint32_t num_symbols, FloxRegistryHandle registry,
                                          const FloxStrategyCallbacks* callbacks)
{
  FloxStrategyCallbacks cbs = callbacks ? *callbacks : FloxStrategyCallbacks{};
  return flox_strategy_create(id, symbols, num_symbols, registry, cbs);
}

void flox_strategy_destroy(FloxStrategyHandle strategy)
{
  delete toStrategy(strategy);
}

void flox_strategy_replace_callbacks(FloxStrategyHandle strategy, FloxStrategyCallbacks callbacks)
{
  if (!strategy)
  {
    return;
  }
  toStrategy(strategy)->replaceCallbacks(callbacks);
}

void flox_strategy_replace_callbacks_p(FloxStrategyHandle strategy,
                                       const FloxStrategyCallbacks* callbacks)
{
  FloxStrategyCallbacks cbs = callbacks ? *callbacks : FloxStrategyCallbacks{};
  flox_strategy_replace_callbacks(strategy, cbs);
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

void flox_indicator_adf(const double* input, size_t len, size_t max_lag, const char* regression,
                        double* test_stat_out, double* p_value_out, size_t* used_lag_out)
{
  std::string reg = regression ? regression : "c";
  auto r = indicator::adf(std::span<const double>(input, len), max_lag, reg);
  if (test_stat_out)
  {
    *test_stat_out = r.test_stat;
  }
  if (p_value_out)
  {
    *p_value_out = r.p_value;
  }
  if (used_lag_out)
  {
    *used_lag_out = r.used_lag;
  }
}

void flox_indicator_autocorrelation(const double* input, size_t len, size_t window, size_t lag,
                                    double* output)
{
  indicator::AutoCorrelation ind(window, lag);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
}

// ============================================================
// Targets
// ============================================================

void flox_target_future_return(const double* close, size_t len, size_t horizon, double* output)
{
  target::FutureReturn t(horizon);
  t.compute(std::span<const double>(close, len), std::span<double>(output, len));
}

void flox_target_future_ctc_volatility(const double* close, size_t len, size_t horizon,
                                       double* output)
{
  target::FutureCTCVolatility t(horizon);
  t.compute(std::span<const double>(close, len), std::span<double>(output, len));
}

void flox_target_future_linear_slope(const double* close, size_t len, size_t horizon,
                                     double* output)
{
  target::FutureLinearSlope t(horizon);
  t.compute(std::span<const double>(close, len), std::span<double>(output, len));
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
// IndicatorGraph — streaming methods on the same handle.
// ============================================================

void flox_indicator_graph_step(FloxIndicatorGraphHandle g, uint32_t symbol, double open,
                               double high, double low, double close, double volume)
{
  auto* impl = static_cast<FloxGraphImpl*>(g);
  Bar bar;
  bar.open = Price::fromDouble(open);
  bar.high = Price::fromDouble(high);
  bar.low = Price::fromDouble(low);
  bar.close = Price::fromDouble(close);
  bar.volume = Volume::fromDouble(volume);
  impl->graph.step(static_cast<SymbolId>(symbol), bar);
}

double flox_indicator_graph_current(FloxIndicatorGraphHandle g, uint32_t symbol, const char* name)
{
  return static_cast<FloxGraphImpl*>(g)->graph.current(static_cast<SymbolId>(symbol), name);
}

uint32_t flox_indicator_graph_bar_count(FloxIndicatorGraphHandle g, uint32_t symbol)
{
  return static_cast<uint32_t>(
      static_cast<FloxGraphImpl*>(g)->graph.barCount(static_cast<SymbolId>(symbol)));
}

void flox_indicator_graph_reset(FloxIndicatorGraphHandle g, uint32_t symbol)
{
  static_cast<FloxGraphImpl*>(g)->graph.reset(static_cast<SymbolId>(symbol));
}

void flox_indicator_graph_reset_all(FloxIndicatorGraphHandle g)
{
  static_cast<FloxGraphImpl*>(g)->graph.resetAll();
}

// ── Deprecated streaming-graph shim — forwards to the unified API ──

FloxStreamingGraphHandle flox_streaming_graph_create(void) { return flox_indicator_graph_create(); }

void flox_streaming_graph_destroy(FloxStreamingGraphHandle sg)
{
  flox_indicator_graph_destroy(sg);
}

void flox_streaming_graph_add_node(FloxStreamingGraphHandle sg, const char* name,
                                   const char* const* deps, size_t num_deps, FloxGraphNodeFn fn,
                                   void* user_data)
{
  flox_indicator_graph_add_node(sg, name, deps, num_deps, fn, user_data);
}

void flox_streaming_graph_step(FloxStreamingGraphHandle sg, uint32_t symbol, double open,
                               double high, double low, double close, double volume)
{
  flox_indicator_graph_step(sg, symbol, open, high, low, close, volume);
}

double flox_streaming_graph_current(FloxStreamingGraphHandle sg, uint32_t symbol, const char* name)
{
  return flox_indicator_graph_current(sg, symbol, name);
}

uint32_t flox_streaming_graph_bar_count(FloxStreamingGraphHandle sg, uint32_t symbol)
{
  return flox_indicator_graph_bar_count(sg, symbol);
}

void flox_streaming_graph_reset(FloxStreamingGraphHandle sg, uint32_t symbol)
{
  flox_indicator_graph_reset(sg, symbol);
}

void flox_streaming_graph_reset_all(FloxStreamingGraphHandle sg)
{
  flox_indicator_graph_reset_all(sg);
}

const double* flox_streaming_graph_close(FloxStreamingGraphHandle sg, uint32_t symbol,
                                         size_t* len_out)
{
  return flox_indicator_graph_close(sg, symbol, len_out);
}
const double* flox_streaming_graph_high(FloxStreamingGraphHandle sg, uint32_t symbol,
                                        size_t* len_out)
{
  return flox_indicator_graph_high(sg, symbol, len_out);
}
const double* flox_streaming_graph_low(FloxStreamingGraphHandle sg, uint32_t symbol,
                                       size_t* len_out)
{
  return flox_indicator_graph_low(sg, symbol, len_out);
}
const double* flox_streaming_graph_volume(FloxStreamingGraphHandle sg, uint32_t symbol,
                                          size_t* len_out)
{
  return flox_indicator_graph_volume(sg, symbol, len_out);
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

struct FloxSimulatedExecutorImpl
{
  SimulatedClock clock;
  SimulatedExecutor executor;
  FloxSimulatedExecutorImpl() : executor(clock) {}
};

FloxSimulatedExecutorHandle flox_simulated_executor_create(void)
{
  return new FloxSimulatedExecutorImpl();
}

void flox_simulated_executor_destroy(FloxSimulatedExecutorHandle executor)
{
  delete static_cast<FloxSimulatedExecutorImpl*>(executor);
}

void flox_simulated_executor_submit_order(FloxSimulatedExecutorHandle h, uint64_t id, uint8_t side, double price,
                                          double quantity, uint8_t order_type, uint32_t symbol)
{
  auto* impl = static_cast<FloxSimulatedExecutorImpl*>(h);
  Order order{};
  order.id = id;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(quantity);
  order.type = static_cast<OrderType>(order_type);
  order.symbol = symbol;
  impl->executor.submitOrder(order);
}

void flox_simulated_executor_submit_order_ex(FloxSimulatedExecutorHandle h, uint64_t id,
                                             uint8_t side, double price, double quantity,
                                             uint8_t order_type, uint32_t symbol,
                                             uint8_t tif, uint8_t reduce_only,
                                             int64_t expires_at_ns)
{
  auto* impl = static_cast<FloxSimulatedExecutorImpl*>(h);
  Order order{};
  order.id = id;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(quantity);
  order.type = static_cast<OrderType>(order_type);
  order.symbol = symbol;
  order.timeInForce = static_cast<TimeInForce>(tif);
  order.flags.reduceOnly = reduce_only ? 1 : 0;
  if (expires_at_ns > 0)
  {
    order.expiresAfter = TimePoint(std::chrono::nanoseconds(expires_at_ns));
  }
  impl->executor.submitOrder(order);
}

void flox_simulated_executor_cancel_order(FloxSimulatedExecutorHandle h, uint64_t order_id)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.cancelOrder(order_id);
}

void flox_simulated_executor_cancel_all(FloxSimulatedExecutorHandle h, uint32_t symbol)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.cancelAllOrders(symbol);
}

void flox_simulated_executor_on_bar(FloxSimulatedExecutorHandle h, uint32_t symbol, double close_price)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBar(symbol, Price::fromDouble(close_price));
}

void flox_simulated_executor_on_trade(FloxSimulatedExecutorHandle h, uint32_t symbol, double price, uint8_t is_buy)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onTrade(symbol, Price::fromDouble(price),
                                                               is_buy != 0);
}

void flox_simulated_executor_advance_clock(FloxSimulatedExecutorHandle h, int64_t timestamp_ns)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->clock.advanceTo(timestamp_ns);
}

uint32_t flox_simulated_executor_fill_count(FloxSimulatedExecutorHandle h)
{
  return static_cast<uint32_t>(static_cast<FloxSimulatedExecutorImpl*>(h)->executor.fills().size());
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
// Multi-timeframe alignment helpers
// ============================================================

static void writeFloxBar(const Bar& bar, FloxBar* out)
{
  out->start_time_ns = bar.startTime.time_since_epoch().count();
  out->end_time_ns = bar.endTime.time_since_epoch().count();
  out->open_raw = bar.open.raw();
  out->high_raw = bar.high.raw();
  out->low_raw = bar.low.raw();
  out->close_raw = bar.close.raw();
  out->volume_raw = bar.volume.raw();
  out->buy_volume_raw = bar.buyVolume.raw();
  out->trade_count = static_cast<uint32_t>(bar.tradeCount.raw());
}

uint8_t flox_strategy_last_closed_bar(FloxStrategyHandle s, uint32_t symbol,
                                      uint8_t bar_type, uint64_t param, FloxBar* out)
{
  auto bar = toStrategy(s)->lastClosedBar(symbol, static_cast<BarType>(bar_type), param);
  if (!bar || !out)
  {
    return 0;
  }
  writeFloxBar(*bar, out);
  return 1;
}

uint32_t flox_strategy_last_n_closed_bars(FloxStrategyHandle s, uint32_t symbol,
                                          uint8_t bar_type, uint64_t param,
                                          FloxBar* bars_out, uint32_t max_bars)
{
  if (!bars_out || max_bars == 0)
  {
    return 0;
  }
  auto bars = toStrategy(s)->lastNClosedBars(symbol, static_cast<BarType>(bar_type),
                                             param, static_cast<size_t>(max_bars));
  uint32_t count = static_cast<uint32_t>(std::min<size_t>(bars.size(), max_bars));
  for (uint32_t i = 0; i < count; ++i)
  {
    writeFloxBar(bars[i], &bars_out[i]);
  }
  return count;
}

uint32_t flox_strategy_get_bar_ring_capacity(FloxStrategyHandle s)
{
  return static_cast<uint32_t>(toStrategy(s)->barRingCapacity());
}

void flox_strategy_set_bar_ring_capacity(FloxStrategyHandle s, uint32_t capacity)
{
  toStrategy(s)->setBarRingCapacity(static_cast<size_t>(capacity));
}

// ============================================================
// Multi-leg order group (W15-T004)
// ============================================================

#include "flox/execution/order_group.h"

static OrderGroup* toOrderGroup(FloxOrderGroupHandle h) { return static_cast<OrderGroup*>(h); }

FloxOrderGroupHandle flox_order_group_create(uint64_t parent_signal_id, uint8_t policy)
{
  return new OrderGroup(parent_signal_id, static_cast<OrderGroupPolicy>(policy));
}

void flox_order_group_destroy(FloxOrderGroupHandle h)
{
  delete toOrderGroup(h);
}

uint32_t flox_order_group_add_market_leg(FloxOrderGroupHandle h, uint32_t symbol,
                                         uint8_t side, int64_t qty_raw)
{
  return static_cast<uint32_t>(
      toOrderGroup(h)->addMarketLeg(symbol, side, Quantity::fromRaw(qty_raw)));
}

uint32_t flox_order_group_add_limit_leg(FloxOrderGroupHandle h, uint32_t symbol,
                                        uint8_t side, int64_t price_raw,
                                        int64_t qty_raw)
{
  return static_cast<uint32_t>(toOrderGroup(h)->addLimitLeg(
      symbol, side, Price::fromRaw(price_raw), Quantity::fromRaw(qty_raw)));
}

uint32_t flox_order_group_leg_count(FloxOrderGroupHandle h)
{
  return static_cast<uint32_t>(toOrderGroup(h)->legCount());
}

uint8_t flox_order_group_leg_state(FloxOrderGroupHandle h, uint32_t leg_index)
{
  return static_cast<uint8_t>(toOrderGroup(h)->leg(leg_index).state);
}

int64_t flox_order_group_leg_filled_raw(FloxOrderGroupHandle h, uint32_t leg_index)
{
  return toOrderGroup(h)->leg(leg_index).filledQty.raw();
}

uint64_t flox_order_group_leg_order_id(FloxOrderGroupHandle h, uint32_t leg_index)
{
  return toOrderGroup(h)->leg(leg_index).orderId;
}

void flox_order_group_record_submit(FloxOrderGroupHandle h, uint32_t leg_index,
                                    uint64_t order_id)
{
  toOrderGroup(h)->recordSubmit(leg_index, order_id);
}

void flox_order_group_record_fill(FloxOrderGroupHandle h, uint32_t leg_index,
                                  int64_t cumulative_qty_raw)
{
  toOrderGroup(h)->recordFill(leg_index, Quantity::fromRaw(cumulative_qty_raw));
}

void flox_order_group_record_cancel(FloxOrderGroupHandle h, uint32_t leg_index)
{
  toOrderGroup(h)->recordCancel(leg_index);
}

void flox_order_group_record_failure(FloxOrderGroupHandle h, uint32_t leg_index)
{
  toOrderGroup(h)->recordFailure(leg_index);
}

void flox_order_group_record_replace_accepted(FloxOrderGroupHandle h, uint32_t leg_index,
                                              uint64_t new_order_id)
{
  toOrderGroup(h)->recordReplaceAccepted(leg_index, new_order_id);
}

void flox_order_group_record_replace_rejected(FloxOrderGroupHandle h, uint32_t leg_index)
{
  toOrderGroup(h)->recordReplaceRejected(leg_index);
}

uint32_t flox_order_group_find_leg_by_order_id(FloxOrderGroupHandle h, uint64_t order_id)
{
  auto idx = toOrderGroup(h)->findLegByOrderId(order_id);
  if (idx.has_value())
  {
    return static_cast<uint32_t>(*idx);
  }
  return UINT32_MAX;
}

uint8_t flox_order_group_state(FloxOrderGroupHandle h)
{
  return static_cast<uint8_t>(toOrderGroup(h)->state());
}

void flox_order_group_mark_action_dispatched(FloxOrderGroupHandle h, uint32_t leg_index,
                                             uint8_t kind)
{
  toOrderGroup(h)->markActionDispatched(leg_index,
                                        static_cast<OrderGroupAction::Kind>(kind));
}

void flox_order_group_set_risk_limits(FloxOrderGroupHandle h, int64_t max_gross_notional_raw,
                                      double max_concentration_pct, int64_t max_leg_qty_raw)
{
  GroupRiskLimits limits;
  limits.maxGrossNotional = Quantity::fromRaw(max_gross_notional_raw);
  limits.maxConcentrationPct = max_concentration_pct;
  limits.maxLegQty = Quantity::fromRaw(max_leg_qty_raw);
  toOrderGroup(h)->setRiskLimits(limits);
}

namespace
{
inline void copy_truncated(const std::string& src, char* out, size_t capacity)
{
  if (!out || capacity == 0)
  {
    return;
  }
  size_t n = std::min(src.size(), capacity - 1);
  std::memcpy(out, src.data(), n);
  out[n] = '\0';
}
}  // namespace

uint8_t flox_order_group_precheck_submission(FloxOrderGroupHandle h, double equity,
                                             const int64_t* market_ref_prices_raw,
                                             uint32_t market_ref_prices_len, char* rule_out,
                                             size_t rule_capacity, char* detail_out,
                                             size_t detail_capacity)
{
  std::vector<Price> prices;
  prices.reserve(market_ref_prices_len);
  for (uint32_t i = 0; i < market_ref_prices_len; ++i)
  {
    prices.push_back(Price::fromRaw(market_ref_prices_raw[i]));
  }
  auto breach = toOrderGroup(h)->precheckSubmission(equity, prices);
  if (!breach.denied)
  {
    if (rule_out && rule_capacity > 0)
    {
      rule_out[0] = '\0';
    }
    if (detail_out && detail_capacity > 0)
    {
      detail_out[0] = '\0';
    }
    return 0;
  }
  copy_truncated(breach.rule, rule_out, rule_capacity);
  copy_truncated(breach.detail, detail_out, detail_capacity);
  return 1;
}

void flox_order_group_set_pair_latency_budget_ns(FloxOrderGroupHandle h, int64_t budget_ns)
{
  toOrderGroup(h)->setPairLatencyBudgetNs(budget_ns);
}

uint8_t flox_order_group_pair_latency_decision(FloxOrderGroupHandle h,
                                               int64_t leader_submit_ts_ns,
                                               int64_t leader_ack_ts_ns,
                                               uint8_t ack_received)
{
  return static_cast<uint8_t>(toOrderGroup(h)->pairLatencyDecision(
      leader_submit_ts_ns, leader_ack_ts_ns, ack_received != 0));
}

uint32_t flox_order_group_recommended_actions(FloxOrderGroupHandle h,
                                              int64_t* actions_out,
                                              uint32_t max_actions)
{
  if (!actions_out || max_actions == 0)
  {
    return 0;
  }
  auto actions = toOrderGroup(h)->recommendedActions();
  uint32_t n = static_cast<uint32_t>(std::min<size_t>(actions.size(), max_actions));
  for (uint32_t i = 0; i < n; ++i)
  {
    const auto& a = actions[i];
    int64_t* slot = actions_out + i * 5;
    slot[0] = static_cast<int64_t>(a.kind);
    slot[1] = static_cast<int64_t>(a.legIndex);
    if (a.kind == OrderGroupAction::Kind::CancelLeg)
    {
      slot[2] = static_cast<int64_t>(a.orderId);
      slot[3] = 0;
      slot[4] = 0;
    }
    else
    {
      slot[2] = static_cast<int64_t>(a.symbol);
      slot[3] = static_cast<int64_t>(a.side);
      slot[4] = a.qty.raw();
    }
  }
  return n;
}

// ============================================================
// Multi-feed clock (W6-T021)
// ============================================================

#include "flox/feed/multi_feed_clock.h"

namespace
{

struct FeedClockState
{
  MultiFeedClock clock;
  FeedClockSnapshot last;

  FeedClockState(std::vector<SymbolId> symbols, FeedClockPolicy policy, int64_t timeoutMs,
                 SymbolId leader, int64_t budgetMs)
      : clock(std::move(symbols), policy, timeoutMs, leader, budgetMs)
  {
  }
};

}  // namespace

static FeedClockState* toFeedClock(FloxFeedClockHandle h)
{
  return static_cast<FeedClockState*>(h);
}

FloxFeedClockHandle flox_feed_clock_create(const uint32_t* symbols, uint32_t symbol_count,
                                           uint8_t policy, int64_t timeout_ms,
                                           uint32_t leader_symbol,
                                           int64_t staleness_budget_ms)
{
  std::vector<SymbolId> sv;
  sv.reserve(symbol_count);
  for (uint32_t i = 0; i < symbol_count; ++i)
  {
    sv.push_back(symbols[i]);
  }
  return new FeedClockState(std::move(sv), static_cast<FeedClockPolicy>(policy), timeout_ms,
                            static_cast<SymbolId>(leader_symbol), staleness_budget_ms);
}

void flox_feed_clock_destroy(FloxFeedClockHandle h)
{
  delete toFeedClock(h);
}

uint32_t flox_feed_clock_symbol_count(FloxFeedClockHandle h)
{
  return static_cast<uint32_t>(toFeedClock(h)->clock.symbolCount());
}

uint32_t flox_feed_clock_symbol_at(FloxFeedClockHandle h, uint32_t index)
{
  auto& last = toFeedClock(h)->last;
  if (last.symbols.empty())
  {
    // Snapshot not yet populated; tick once to materialize symbols.
    // Use ts=0 + an out-of-band symbol so the tick records nothing.
    toFeedClock(h)->last = toFeedClock(h)->clock.tick(0, 0);
  }
  if (index >= toFeedClock(h)->last.symbols.size())
  {
    return 0;
  }
  return toFeedClock(h)->last.symbols[index];
}

uint8_t flox_feed_clock_tick(FloxFeedClockHandle h, int64_t ts_ns, uint32_t symbol)
{
  auto* st = toFeedClock(h);
  st->last = st->clock.tick(ts_ns, static_cast<SymbolId>(symbol));
  return st->last.fired ? 1 : 0;
}

uint8_t flox_feed_clock_last_fired(FloxFeedClockHandle h)
{
  return toFeedClock(h)->last.fired ? 1 : 0;
}

uint32_t flox_feed_clock_last_triggered_by(FloxFeedClockHandle h)
{
  return static_cast<uint32_t>(toFeedClock(h)->last.triggeredBy);
}

int64_t flox_feed_clock_last_seen_at(FloxFeedClockHandle h, uint32_t index)
{
  auto& last = toFeedClock(h)->last;
  if (index >= last.lastTsNs.size())
  {
    return 0;
  }
  return last.lastTsNs[index];
}

int64_t flox_feed_clock_staleness_at(FloxFeedClockHandle h, uint32_t index)
{
  auto& last = toFeedClock(h)->last;
  if (index >= last.stalenessNs.size())
  {
    return 0;
  }
  return last.stalenessNs[index];
}

void flox_feed_clock_reset(FloxFeedClockHandle h)
{
  toFeedClock(h)->clock.reset();
  toFeedClock(h)->last = FeedClockSnapshot{};
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
// Order journey tracer
// ============================================================

namespace
{
flox::OrderJourneyTracer* asTracer(FloxOrderJourneyTracerHandle h)
{
  return static_cast<flox::OrderJourneyTracer*>(h);
}

FloxOrderTraceRow toCapiRow(const flox::OrderTraceRecord& r)
{
  FloxOrderTraceRow row{};
  row.order_id = r.orderId;
  row.seq = r.seq;
  row.status = r.status;
  row.is_maker = r.isMaker;
  row.ts_ns = r.tsNs;
  row.fill_qty_raw = r.fillQtyRaw;
  row.fill_price_raw = r.fillPriceRaw;
  row.queue_ahead_raw = r.queueAheadRaw;
  row.queue_total_raw = r.queueTotalRaw;
  row.submitted_at_ns = r.timestamps.submittedAtNs;
  row.accepted_at_ns = r.timestamps.acceptedAtNs;
  row.first_fill_at_ns = r.timestamps.firstFillAtNs;
  row.last_fill_at_ns = r.timestamps.lastFillAtNs;
  row.canceled_at_ns = r.timestamps.canceledAtNs;
  row.rejected_at_ns = r.timestamps.rejectedAtNs;
  row.triggered_at_ns = r.timestamps.triggeredAtNs;
  row.expired_at_ns = r.timestamps.expiredAtNs;
  return row;
}
}  // namespace

FloxOrderJourneyTracerHandle flox_order_journey_tracer_create(
    uint64_t max_orders, uint64_t max_records_per_order, double sample_rate,
    uint64_t sample_salt)
{
  flox::OrderJourneyTracer::Config cfg{};
  cfg.maxOrders = max_orders;
  cfg.maxRecordsPerOrder = max_records_per_order;
  cfg.sampleRate = sample_rate;
  cfg.sampleSalt = sample_salt;
  return new flox::OrderJourneyTracer(cfg);
}

void flox_order_journey_tracer_destroy(FloxOrderJourneyTracerHandle h)
{
  delete asTracer(h);
}

uint64_t flox_order_journey_tracer_order_count(FloxOrderJourneyTracerHandle h)
{
  return asTracer(h) ? asTracer(h)->orderCount() : 0u;
}

uint64_t flox_order_journey_tracer_record_count(FloxOrderJourneyTracerHandle h)
{
  return asTracer(h) ? asTracer(h)->recordCount() : 0u;
}

double flox_order_journey_tracer_median_ack_latency_ns(FloxOrderJourneyTracerHandle h)
{
  return asTracer(h) ? asTracer(h)->medianAckLatencyNs() : 0.0;
}

double flox_order_journey_tracer_median_time_to_first_fill_ns(
    FloxOrderJourneyTracerHandle h)
{
  return asTracer(h) ? asTracer(h)->medianTimeToFirstFillNs() : 0.0;
}

double flox_order_journey_tracer_maker_fill_ratio(FloxOrderJourneyTracerHandle h)
{
  return asTracer(h) ? asTracer(h)->makerFillRatio() : 0.0;
}

double flox_order_journey_tracer_cancel_race_loss_rate(FloxOrderJourneyTracerHandle h)
{
  return asTracer(h) ? asTracer(h)->cancelRaceLossRate() : 0.0;
}

uint64_t flox_order_journey_tracer_result(FloxOrderJourneyTracerHandle h,
                                          FloxOrderTraceRow* out, uint64_t max_rows)
{
  auto* tracer = asTracer(h);
  if (!tracer)
  {
    return 0u;
  }
  auto rows = tracer->result();
  if (out == nullptr || max_rows == 0)
  {
    return rows.size();
  }
  const uint64_t n = std::min<uint64_t>(rows.size(), max_rows);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i] = toCapiRow(rows[i]);
  }
  return n;
}

uint64_t flox_order_journey_tracer_journey(FloxOrderJourneyTracerHandle h,
                                           uint64_t order_id, FloxOrderTraceRow* out,
                                           uint64_t max_rows)
{
  auto* tracer = asTracer(h);
  if (!tracer)
  {
    return 0u;
  }
  auto rows = tracer->journey(static_cast<flox::OrderId>(order_id));
  if (out == nullptr || max_rows == 0)
  {
    return rows.size();
  }
  const uint64_t n = std::min<uint64_t>(rows.size(), max_rows);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i] = toCapiRow(rows[i]);
  }
  return n;
}

void flox_order_journey_tracer_clear(FloxOrderJourneyTracerHandle h)
{
  if (auto* tracer = asTracer(h))
  {
    tracer->clear();
  }
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

uint32_t flox_simulated_executor_get_fills(FloxSimulatedExecutorHandle h, FloxFill* fills_out, uint32_t max_fills)
{
  auto& fills = static_cast<FloxSimulatedExecutorImpl*>(h)->executor.fills();
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

uint8_t flox_data_writer_write_book(FloxDataWriterHandle h,
                                    int64_t exchange_ts_ns,
                                    int64_t recv_ts_ns,
                                    int64_t seq,
                                    uint32_t symbol_id,
                                    uint8_t is_snapshot,
                                    const FloxBookLevel* bids, uint32_t n_bids,
                                    const FloxBookLevel* asks, uint32_t n_asks)
{
  replay::BookRecordHeader header{};
  header.exchange_ts_ns = exchange_ts_ns;
  header.recv_ts_ns = recv_ts_ns;
  header.seq = seq;
  header.symbol_id = symbol_id;
  header.bid_count = static_cast<uint16_t>(n_bids);
  header.ask_count = static_cast<uint16_t>(n_asks);
  header.type = is_snapshot ? 0 : 1;

  // FloxBookLevel and replay::BookLevel are layout-compatible
  // (int64 price_raw, int64 qty_raw). Cast is intentional.
  auto* bid_levels = reinterpret_cast<const replay::BookLevel*>(bids);
  auto* ask_levels = reinterpret_cast<const replay::BookLevel*>(asks);
  std::span<const replay::BookLevel> bid_span(bid_levels, n_bids);
  std::span<const replay::BookLevel> ask_span(ask_levels, n_asks);

  return static_cast<replay::BinaryLogWriter*>(h)->writeBook(header, bid_span, ask_span) ? 1 : 0;
}

uint64_t flox_data_writer_write_books(FloxDataWriterHandle h,
                                      const FloxBookUpdateHeader* headers,
                                      uint64_t n_events,
                                      const FloxLevel* levels,
                                      uint64_t /*total_levels*/)
{
  auto* writer = static_cast<replay::BinaryLogWriter*>(h);
  if (!writer || !headers)
  {
    return 0;
  }

  // FloxLevel has an extra `side` byte we ignore on write — copy into
  // contiguous replay::BookLevel buffers per event.
  std::vector<replay::BookLevel> scratch;
  uint64_t written = 0;
  for (uint64_t i = 0; i < n_events; ++i)
  {
    const auto& fh = headers[i];
    const uint64_t total = static_cast<uint64_t>(fh.bid_count) + fh.ask_count;
    scratch.clear();
    scratch.reserve(total);
    for (uint64_t k = 0; k < total; ++k)
    {
      const auto& lv = levels[fh.level_offset + k];
      scratch.push_back({lv.price_raw, lv.qty_raw});
    }

    replay::BookRecordHeader rh{};
    rh.exchange_ts_ns = fh.exchange_ts_ns;
    rh.recv_ts_ns = fh.recv_ts_ns;
    rh.seq = fh.seq;
    rh.symbol_id = fh.symbol_id;
    rh.bid_count = fh.bid_count;
    rh.ask_count = fh.ask_count;
    rh.type = (fh.event_type == 2) ? 0 : 1;

    std::span<const replay::BookLevel> bid_span(scratch.data(), fh.bid_count);
    std::span<const replay::BookLevel> ask_span(scratch.data() + fh.bid_count, fh.ask_count);
    if (writer->writeBook(rh, bid_span, ask_span))
    {
      ++written;
    }
  }
  return written;
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

void flox_stat_whites_reality_check(const double* returns, size_t num_strategies,
                                    size_t num_periods, uint32_t num_bootstrap,
                                    double avg_block_size, double* p_value_out,
                                    double* best_stat_out, int32_t* best_index_out)
{
  auto result = flox::stats::whitesRealityCheck(
      returns, num_strategies, num_periods, num_bootstrap, avg_block_size);
  if (p_value_out)
  {
    *p_value_out = result.pValue;
  }
  if (best_stat_out)
  {
    *best_stat_out = result.bestStat;
  }
  if (best_index_out)
  {
    *best_index_out = result.bestIndex;
  }
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

void flox_simulated_executor_set_default_slippage(FloxSimulatedExecutorHandle h, int32_t model, int32_t ticks,
                                                  double tick_size, double bps, double impact_coeff)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setDefaultSlippage(
      makeSlippageProfile(model, ticks, tick_size, bps, impact_coeff));
}

void flox_simulated_executor_set_symbol_slippage(FloxSimulatedExecutorHandle h, uint32_t symbol, int32_t model,
                                                 int32_t ticks, double tick_size, double bps,
                                                 double impact_coeff)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setSymbolSlippage(
      symbol, makeSlippageProfile(model, ticks, tick_size, bps, impact_coeff));
}

void flox_simulated_executor_set_queue_model(FloxSimulatedExecutorHandle h, int32_t model, uint32_t depth)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setQueueModel(
      static_cast<QueueModel>(model), depth);
}

void flox_simulated_executor_set_submit_ack_latency(FloxSimulatedExecutorHandle h,
                                                    int64_t latency_ns, int64_t jitter_ns)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setSubmitAckLatency(latency_ns, jitter_ns);
}

void flox_simulated_executor_set_cancel_ack_latency(FloxSimulatedExecutorHandle h,
                                                    int64_t latency_ns, int64_t jitter_ns)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setCancelAckLatency(latency_ns, jitter_ns);
}

void flox_simulated_executor_set_replace_ack_latency(FloxSimulatedExecutorHandle h,
                                                     int64_t latency_ns, int64_t jitter_ns)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setReplaceAckLatency(latency_ns, jitter_ns);
}

void flox_simulated_executor_apply_latency_profile(FloxSimulatedExecutorHandle h,
                                                   const char* profile_name)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.applyLatencyProfile(profile_name);
}

// Latency distribution handle bridging.

namespace
{
inline LatencyDistribution* toDist(FloxLatencyDistributionHandle h)
{
  return static_cast<LatencyDistribution*>(h);
}
}  // namespace

FloxLatencyDistributionHandle flox_latency_distribution_create(void)
{
  return new LatencyDistribution();
}

void flox_latency_distribution_destroy(FloxLatencyDistributionHandle h)
{
  delete toDist(h);
}

void flox_latency_distribution_set_constant(FloxLatencyDistributionHandle h, int64_t ns)
{
  const double rho = toDist(h)->burstCorrelation();
  *toDist(h) = LatencyDistribution::constant(ns);
  toDist(h)->setBurstCorrelation(rho);
}

void flox_latency_distribution_set_uniform(FloxLatencyDistributionHandle h, int64_t lo_ns,
                                           int64_t hi_ns)
{
  const double rho = toDist(h)->burstCorrelation();
  *toDist(h) = LatencyDistribution::uniform(lo_ns, hi_ns);
  toDist(h)->setBurstCorrelation(rho);
}

void flox_latency_distribution_set_lognormal(FloxLatencyDistributionHandle h,
                                             int64_t median_ns, double sigma)
{
  const double rho = toDist(h)->burstCorrelation();
  *toDist(h) = LatencyDistribution::lognormal(median_ns, sigma);
  toDist(h)->setBurstCorrelation(rho);
}

void flox_latency_distribution_set_empirical(FloxLatencyDistributionHandle h,
                                             const int64_t* samples_ns, uint32_t n_samples)
{
  const double rho = toDist(h)->burstCorrelation();
  std::vector<int64_t> samples;
  samples.reserve(n_samples);
  for (uint32_t i = 0; i < n_samples; ++i)
  {
    samples.push_back(samples_ns ? samples_ns[i] : 0);
  }
  *toDist(h) = LatencyDistribution::empirical(samples);
  toDist(h)->setBurstCorrelation(rho);
}

void flox_latency_distribution_set_burst_correlation(FloxLatencyDistributionHandle h, double rho)
{
  toDist(h)->setBurstCorrelation(rho);
}

int64_t flox_latency_distribution_median_ns(FloxLatencyDistributionHandle h)
{
  return toDist(h)->medianNs();
}

void flox_simulated_executor_set_submit_ack_latency_distribution(
    FloxSimulatedExecutorHandle h, FloxLatencyDistributionHandle dist)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setSubmitAckLatencyDistribution(
      *toDist(dist));
}

void flox_simulated_executor_set_cancel_ack_latency_distribution(
    FloxSimulatedExecutorHandle h, FloxLatencyDistributionHandle dist)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setCancelAckLatencyDistribution(
      *toDist(dist));
}

void flox_simulated_executor_set_replace_ack_latency_distribution(
    FloxSimulatedExecutorHandle h, FloxLatencyDistributionHandle dist)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setReplaceAckLatencyDistribution(
      *toDist(dist));
}

// ====== Rate-limit policy bridging ======

#include "flox/backtest/rate_limit_policy.h"

namespace
{
inline flox::RateLimitPolicy* toRateLimit(FloxRateLimitPolicyHandle h)
{
  return static_cast<flox::RateLimitPolicy*>(h);
}
}  // namespace

extern "C" FloxRateLimitPolicyHandle flox_rate_limit_policy_create(void)
{
  return new flox::RateLimitPolicy();
}

extern "C" void flox_rate_limit_policy_destroy(FloxRateLimitPolicyHandle h)
{
  delete toRateLimit(h);
}

extern "C" void flox_rate_limit_policy_add_bucket(FloxRateLimitPolicyHandle h,
                                                  const char* name, int64_t window_ns,
                                                  uint32_t capacity, uint32_t submit_w,
                                                  uint32_t cancel_w, uint32_t replace_w)
{
  toRateLimit(h)->addBucket(name ? std::string(name) : std::string("bucket"), window_ns,
                            capacity, submit_w, cancel_w, replace_w);
}

extern "C" void flox_rate_limit_policy_set_ban(FloxRateLimitPolicyHandle h,
                                               uint32_t after_consecutive_rejects,
                                               int64_t ban_duration_ns)
{
  toRateLimit(h)->setBan(after_consecutive_rejects, ban_duration_ns);
}

extern "C" void flox_rate_limit_policy_load_profile(FloxRateLimitPolicyHandle h,
                                                    const char* name)
{
  if (!h || !name)
  {
    return;
  }
  std::string n = name;
  if (n == "binance_um_futures")
  {
    *toRateLimit(h) = flox::RateLimitPolicy::binance_um_futures();
  }
  else if (n == "bybit_linear")
  {
    *toRateLimit(h) = flox::RateLimitPolicy::bybit_linear();
  }
  else if (n == "okx_swap")
  {
    *toRateLimit(h) = flox::RateLimitPolicy::okx_swap();
  }
  else if (n == "deribit")
  {
    *toRateLimit(h) = flox::RateLimitPolicy::deribit();
  }
}

extern "C" int64_t flox_rate_limit_policy_ban_until_ns(FloxRateLimitPolicyHandle h)
{
  return toRateLimit(h)->banUntilNs();
}

extern "C" uint32_t flox_rate_limit_policy_consecutive_rejects(FloxRateLimitPolicyHandle h)
{
  return toRateLimit(h)->consecutiveRejects();
}

extern "C" uint32_t flox_rate_limit_policy_bucket_state(FloxRateLimitPolicyHandle h,
                                                        int64_t now_ns, int64_t* out_buf,
                                                        uint32_t max_buckets)
{
  auto states = toRateLimit(h)->bucketStates(now_ns);
  if (out_buf == nullptr || max_buckets == 0)
  {
    return static_cast<uint32_t>(states.size());
  }
  uint32_t n = std::min<uint32_t>(max_buckets, static_cast<uint32_t>(states.size()));
  for (uint32_t i = 0; i < n; ++i)
  {
    out_buf[i * 4 + 0] = states[i].windowNs;
    out_buf[i * 4 + 1] = static_cast<int64_t>(states[i].used);
    out_buf[i * 4 + 2] = static_cast<int64_t>(states[i].capacity);
    out_buf[i * 4 + 3] = 0;
  }
  return n;
}

extern "C" void flox_simulated_executor_set_rate_limit_policy(
    FloxSimulatedExecutorHandle exec_h, FloxRateLimitPolicyHandle policy_h)
{
  static_cast<FloxSimulatedExecutorImpl*>(exec_h)->executor.setRateLimitPolicy(
      *toRateLimit(policy_h));
}

extern "C" void flox_simulated_executor_clear_rate_limit_policy(
    FloxSimulatedExecutorHandle exec_h)
{
  static_cast<FloxSimulatedExecutorImpl*>(exec_h)->executor.clearRateLimitPolicy();
}

void flox_simulated_executor_on_trade_qty(FloxSimulatedExecutorHandle h, uint32_t symbol, double price,
                                          double quantity, uint8_t is_buy)
{
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onTrade(
      symbol, Price::fromDouble(price), Quantity::fromDouble(quantity), is_buy != 0);
}

void flox_simulated_executor_on_best_levels(FloxSimulatedExecutorHandle h, uint32_t symbol, double bid_price,
                                            double bid_qty, double ask_price, double ask_qty)
{
  std::pmr::monotonic_buffer_resource pool(512);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.emplace_back(Price::fromDouble(bid_price), Quantity::fromDouble(bid_qty));
  asks.emplace_back(Price::fromDouble(ask_price), Quantity::fromDouble(ask_qty));
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBookUpdate(symbol, bids, asks);
}

void flox_simulated_executor_on_book_snapshot(FloxSimulatedExecutorHandle h, uint32_t symbol,
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
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBookUpdate(symbol, bids, asks);
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

void flox_backtest_result_ingest_executor(FloxBacktestResultHandle h, FloxSimulatedExecutorHandle eh)
{
  auto* impl = static_cast<FloxBacktestResultImpl*>(h);
  for (const auto& fill : static_cast<FloxSimulatedExecutorImpl*>(eh)->executor.fills())
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

uint32_t flox_backtest_result_trades(FloxBacktestResultHandle h, FloxBacktestTrade* out,
                                     uint32_t max_trades)
{
  const auto& trades = static_cast<FloxBacktestResultImpl*>(h)->result->trades();
  const uint32_t total = static_cast<uint32_t>(trades.size());
  if (!out)
  {
    return total;
  }
  const uint32_t n = (total < max_trades) ? total : max_trades;
  for (uint32_t i = 0; i < n; ++i)
  {
    const auto& t = trades[i];
    out[i].symbol = static_cast<uint32_t>(t.symbol);
    out[i].side = static_cast<uint8_t>(t.side);
    out[i].entry_price = t.entryPrice.toDouble();
    out[i].exit_price = t.exitPrice.toDouble();
    out[i].quantity = t.quantity.toDouble();
    out[i].entry_time_ns = static_cast<int64_t>(t.entryTimeNs);
    out[i].exit_time_ns = static_cast<int64_t>(t.exitTimeNs);
    out[i].pnl = t.pnl.toDouble();
    out[i].fee = t.fee.toDouble();
  }
  return n;
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

// Layout invariants — language bindings (Codon, QuickJS) parse these structs
// from raw byte buffers and depend on exact offsets/sizes.
static_assert(sizeof(FloxBBO) == 64, "FloxBBO must be 64 bytes");
static_assert(sizeof(FloxBookUpdateHeader) == 48, "FloxBookUpdateHeader must be 48 bytes");
static_assert(sizeof(FloxLevel) == 24, "FloxLevel must be 24 bytes");

uint64_t flox_data_reader_read_bbo(FloxDataReaderHandle h, FloxBBO* bbos_out,
                                   uint64_t max_events)
{
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type != replay::EventType::BookSnapshot &&
            ev.type != replay::EventType::BookDelta)
        {
          return true;
        }

        if (bbos_out && count < max_events)
        {
          FloxBBO b{};
          b.exchange_ts_ns = ev.book_header.exchange_ts_ns;
          b.recv_ts_ns = ev.book_header.recv_ts_ns;
          b.seq = ev.book_header.seq;
          b.symbol_id = ev.book_header.symbol_id;
          b.event_type = ev.book_header.type;
          if (!ev.bids.empty())
          {
            b.bid_price_raw = ev.bids[0].price_raw;
            b.bid_qty_raw = ev.bids[0].qty_raw;
          }
          if (!ev.asks.empty())
          {
            b.ask_price_raw = ev.asks[0].price_raw;
            b.ask_qty_raw = ev.asks[0].qty_raw;
          }
          bbos_out[count] = b;
        }
        ++count;
        return !bbos_out || count < max_events;
      });
  return count;
}

uint64_t flox_data_reader_count_book_updates(FloxDataReaderHandle h, uint64_t* total_levels_out)
{
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type == replay::EventType::BookSnapshot ||
            ev.type == replay::EventType::BookDelta)
        {
          ++events;
          levels += ev.bids.size() + ev.asks.size();
        }
        return true;
      });
  if (total_levels_out)
  {
    *total_levels_out = levels;
  }
  return events;
}

uint64_t flox_data_reader_read_book_updates(FloxDataReaderHandle h,
                                            FloxBookUpdateHeader* headers_out,
                                            uint64_t max_events,
                                            FloxLevel* levels_out,
                                            uint64_t max_levels)
{
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels_written = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type != replay::EventType::BookSnapshot &&
            ev.type != replay::EventType::BookDelta)
        {
          return true;
        }

        const uint64_t bid_n = ev.bids.size();
        const uint64_t ask_n = ev.asks.size();
        const uint64_t total = bid_n + ask_n;

        if (headers_out && events < max_events && levels_written + total <= max_levels)
        {
          FloxBookUpdateHeader hdr{};
          hdr.exchange_ts_ns = ev.book_header.exchange_ts_ns;
          hdr.recv_ts_ns = ev.book_header.recv_ts_ns;
          hdr.seq = ev.book_header.seq;
          hdr.level_offset = levels_written;
          hdr.symbol_id = ev.book_header.symbol_id;
          hdr.bid_count = static_cast<uint16_t>(bid_n);
          hdr.ask_count = static_cast<uint16_t>(ask_n);
          hdr.event_type = ev.book_header.type;
          headers_out[events] = hdr;

          for (uint64_t i = 0; i < bid_n; ++i)
          {
            levels_out[levels_written + i] = {ev.bids[i].price_raw, ev.bids[i].qty_raw, 0};
          }
          for (uint64_t i = 0; i < ask_n; ++i)
          {
            levels_out[levels_written + bid_n + i] = {ev.asks[i].price_raw, ev.asks[i].qty_raw, 1};
          }

          levels_written += total;
          ++events;
          return true;
        }

        return false;
      });
  return events;
}

// ── _from variants ─────────────────────────────────────────────
// Mid-stream seek: start iterating from start_ts_ns instead of segment
// beginning. Behaviour is otherwise identical to the matching non-_from
// reader.

uint64_t flox_data_reader_read_trades_from(FloxDataReaderHandle h, int64_t start_ts_ns,
                                           FloxTradeRecord* trades_out, uint64_t max_trades)
{
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type == replay::EventType::Trade)
                        {
                          if (trades_out && count < max_trades)
                          {
                            trades_out[count] = {ev.trade.exchange_ts_ns, ev.trade.recv_ts_ns,
                                                 ev.trade.price_raw, ev.trade.qty_raw,
                                                 ev.trade.trade_id, ev.trade.symbol_id,
                                                 ev.trade.side};
                          }
                          ++count;
                        }
                        return !trades_out || count < max_trades;
                      });
  return count;
}

uint64_t flox_data_reader_read_bbo_from(FloxDataReaderHandle h, int64_t start_ts_ns,
                                        FloxBBO* bbos_out, uint64_t max_events)
{
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type != replay::EventType::BookSnapshot &&
                            ev.type != replay::EventType::BookDelta)
                        {
                          return true;
                        }

                        if (bbos_out && count < max_events)
                        {
                          FloxBBO b{};
                          b.exchange_ts_ns = ev.book_header.exchange_ts_ns;
                          b.recv_ts_ns = ev.book_header.recv_ts_ns;
                          b.seq = ev.book_header.seq;
                          b.symbol_id = ev.book_header.symbol_id;
                          b.event_type = ev.book_header.type;
                          if (!ev.bids.empty())
                          {
                            b.bid_price_raw = ev.bids[0].price_raw;
                            b.bid_qty_raw = ev.bids[0].qty_raw;
                          }
                          if (!ev.asks.empty())
                          {
                            b.ask_price_raw = ev.asks[0].price_raw;
                            b.ask_qty_raw = ev.asks[0].qty_raw;
                          }
                          bbos_out[count] = b;
                        }
                        ++count;
                        return !bbos_out || count < max_events;
                      });
  return count;
}

uint64_t flox_data_reader_count_book_updates_from(FloxDataReaderHandle h, int64_t start_ts_ns,
                                                  uint64_t* total_levels_out)
{
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type == replay::EventType::BookSnapshot ||
                            ev.type == replay::EventType::BookDelta)
                        {
                          ++events;
                          levels += ev.bids.size() + ev.asks.size();
                        }
                        return true;
                      });
  if (total_levels_out)
  {
    *total_levels_out = levels;
  }
  return events;
}

uint64_t flox_data_reader_read_book_updates_from(FloxDataReaderHandle h, int64_t start_ts_ns,
                                                 FloxBookUpdateHeader* headers_out,
                                                 uint64_t max_events, FloxLevel* levels_out,
                                                 uint64_t max_levels)
{
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels_written = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type != replay::EventType::BookSnapshot &&
                            ev.type != replay::EventType::BookDelta)
                        {
                          return true;
                        }

                        const uint64_t bid_n = ev.bids.size();
                        const uint64_t ask_n = ev.asks.size();
                        const uint64_t total = bid_n + ask_n;

                        if (headers_out && events < max_events &&
                            levels_written + total <= max_levels)
                        {
                          FloxBookUpdateHeader hdr{};
                          hdr.exchange_ts_ns = ev.book_header.exchange_ts_ns;
                          hdr.recv_ts_ns = ev.book_header.recv_ts_ns;
                          hdr.seq = ev.book_header.seq;
                          hdr.level_offset = levels_written;
                          hdr.symbol_id = ev.book_header.symbol_id;
                          hdr.bid_count = static_cast<uint16_t>(bid_n);
                          hdr.ask_count = static_cast<uint16_t>(ask_n);
                          hdr.event_type = ev.book_header.type;
                          headers_out[events] = hdr;

                          for (uint64_t i = 0; i < bid_n; ++i)
                          {
                            levels_out[levels_written + i] = {ev.bids[i].price_raw,
                                                              ev.bids[i].qty_raw, 0};
                          }
                          for (uint64_t i = 0; i < ask_n; ++i)
                          {
                            levels_out[levels_written + bid_n + i] = {
                                ev.asks[i].price_raw, ev.asks[i].qty_raw, 1};
                          }

                          levels_written += total;
                          ++events;
                          return true;
                        }

                        return false;
                      });
  return events;
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
// MergedTapeReader — N-tape merged consumption
// ============================================================
namespace capi_impl
{
struct FloxMergedTapeReaderImpl
{
  std::unique_ptr<flox::replay::MergedTapeReader> reader;
  // Cached result of readTrades / readBooks so two-phase count→read
  // doesn't re-merge. Cleared on first read of each kind.
  std::optional<std::vector<flox::replay::MergedTradeRow>> cached_trades;
  std::optional<std::pair<std::vector<flox::replay::MergedBookRow>,
                          std::vector<flox::replay::BookLevel>>>
      cached_books;

  // Owning strings for symbol_table and per-tape paths so the borrowed
  // const char* pointers we hand out stay valid for the reader's lifetime.
  std::vector<std::string> sym_exchanges;
  std::vector<std::string> sym_names;
  std::vector<std::string> tape_paths;
};
}  // namespace capi_impl

FloxMergedTapeReaderHandle
flox_merged_tape_reader_create(const char* const* paths, uint32_t n_paths,
                               int64_t from_ns, int64_t to_ns,
                               const uint32_t* symbol_filter,
                               uint32_t n_filter)
{
  if (!paths || n_paths == 0)
  {
    return nullptr;
  }
  flox::replay::MergedTapeReaderConfig cfg{};
  cfg.tape_dirs.reserve(n_paths);
  for (uint32_t i = 0; i < n_paths; ++i)
  {
    cfg.tape_dirs.emplace_back(paths[i] ? paths[i] : "");
  }
  if (from_ns >= 0)
  {
    cfg.from_ns = from_ns;
  }
  if (to_ns >= 0)
  {
    cfg.to_ns = to_ns;
  }
  if (symbol_filter && n_filter > 0)
  {
    cfg.symbol_filter.assign(symbol_filter, symbol_filter + n_filter);
  }

  try
  {
    auto impl = std::make_unique<capi_impl::FloxMergedTapeReaderImpl>();
    impl->reader =
        std::make_unique<flox::replay::MergedTapeReader>(std::move(cfg));
    // Cache symbol / path strings now so const char* pointers stay stable.
    for (const auto& s : impl->reader->symbols())
    {
      impl->sym_exchanges.push_back(s.exchange);
      impl->sym_names.push_back(s.name);
    }
    for (const auto& t : impl->reader->perTapeStats())
    {
      impl->tape_paths.push_back(t.path.string());
    }
    return static_cast<FloxMergedTapeReaderHandle>(impl.release());
  }
  catch (...)
  {
    // Construction may throw on bad input or overlapping book streams;
    // surface as a NULL handle, leaving caller to decide.
    return nullptr;
  }
}

void flox_merged_tape_reader_destroy(FloxMergedTapeReaderHandle h)
{
  delete static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
}

uint32_t flox_merged_tape_reader_symbol_count(FloxMergedTapeReaderHandle h)
{
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  return static_cast<uint32_t>(impl->reader->symbols().size());
}

uint32_t flox_merged_tape_reader_get_symbols(FloxMergedTapeReaderHandle h,
                                             FloxMergedSymbol* out,
                                             uint32_t max)
{
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  const auto& syms = impl->reader->symbols();
  uint32_t n = std::min(static_cast<uint32_t>(syms.size()), max);
  if (!out)
  {
    return static_cast<uint32_t>(syms.size());
  }
  for (uint32_t i = 0; i < n; ++i)
  {
    out[i].global_id = syms[i].global_id;
    out[i].price_precision = syms[i].price_precision;
    out[i].qty_precision = syms[i].qty_precision;
    out[i]._pad[0] = out[i]._pad[1] = 0;
    out[i].exchange = impl->sym_exchanges[i].c_str();
    out[i].name = impl->sym_names[i].c_str();
  }
  return n;
}

uint32_t flox_merged_tape_reader_tape_count(FloxMergedTapeReaderHandle h)
{
  if (!h)
  {
    return 0;
  }
  return static_cast<uint32_t>(
      static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h)
          ->reader->perTapeStats()
          .size());
}

uint32_t flox_merged_tape_reader_get_tape_stats(FloxMergedTapeReaderHandle h,
                                                FloxMergedTapeStats* out,
                                                uint32_t max)
{
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  const auto& stats = impl->reader->perTapeStats();
  uint32_t n = std::min(static_cast<uint32_t>(stats.size()), max);
  if (!out)
  {
    return static_cast<uint32_t>(stats.size());
  }
  for (uint32_t i = 0; i < n; ++i)
  {
    out[i].first_event_ns = stats[i].first_event_ns;
    out[i].last_event_ns = stats[i].last_event_ns;
    out[i].trades = stats[i].trades;
    out[i].books = stats[i].books;
    out[i].path = impl->tape_paths[i].c_str();
  }
  return n;
}

void flox_merged_tape_reader_time_range(FloxMergedTapeReaderHandle h,
                                        int64_t* min_first_ns_out,
                                        int64_t* max_last_ns_out)
{
  if (!h)
  {
    if (min_first_ns_out)
    {
      *min_first_ns_out = 0;
    }
    if (max_last_ns_out)
    {
      *max_last_ns_out = 0;
    }
    return;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  auto [a, b] = impl->reader->timeRange();
  if (min_first_ns_out)
  {
    *min_first_ns_out = a;
  }
  if (max_last_ns_out)
  {
    *max_last_ns_out = b;
  }
}

uint64_t flox_merged_tape_reader_count_trades(FloxMergedTapeReaderHandle h)
{
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  if (!impl->cached_trades)
  {
    impl->cached_trades = impl->reader->readTrades();
  }
  return impl->cached_trades->size();
}

uint64_t flox_merged_tape_reader_read_trades(FloxMergedTapeReaderHandle h,
                                             FloxTradeRecord* trades_out,
                                             uint64_t max_trades)
{
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  if (!impl->cached_trades)
  {
    impl->cached_trades = impl->reader->readTrades();
  }
  const auto& src = *impl->cached_trades;
  uint64_t n = std::min<uint64_t>(src.size(), max_trades);
  if (!trades_out)
  {
    return src.size();
  }
  for (uint64_t i = 0; i < n; ++i)
  {
    trades_out[i].exchange_ts_ns = src[i].exchange_ts_ns;
    trades_out[i].recv_ts_ns = src[i].recv_ts_ns;
    trades_out[i].price_raw = src[i].price_raw;
    trades_out[i].qty_raw = src[i].qty_raw;
    trades_out[i].trade_id = src[i].trade_id;
    trades_out[i].symbol_id = src[i].global_symbol_id;
    trades_out[i].side = src[i].side;
  }
  return n;
}

uint64_t flox_merged_tape_reader_count_books(FloxMergedTapeReaderHandle h,
                                             uint64_t* total_levels_out)
{
  if (!h)
  {
    if (total_levels_out)
    {
      *total_levels_out = 0;
    }
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  if (!impl->cached_books)
  {
    impl->cached_books = impl->reader->readBooks();
  }
  if (total_levels_out)
  {
    *total_levels_out = impl->cached_books->second.size();
  }
  return impl->cached_books->first.size();
}

uint64_t flox_merged_tape_reader_read_books(FloxMergedTapeReaderHandle h,
                                            FloxBookUpdateHeader* headers_out,
                                            uint64_t max_events,
                                            FloxLevel* levels_out,
                                            uint64_t max_levels)
{
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  if (!impl->cached_books)
  {
    impl->cached_books = impl->reader->readBooks();
  }
  const auto& [rows, levels] = *impl->cached_books;
  uint64_t n_ev = std::min<uint64_t>(rows.size(), max_events);
  uint64_t n_lv = std::min<uint64_t>(levels.size(), max_levels);

  if (headers_out)
  {
    for (uint64_t i = 0; i < n_ev; ++i)
    {
      headers_out[i].exchange_ts_ns = rows[i].exchange_ts_ns;
      headers_out[i].recv_ts_ns = rows[i].recv_ts_ns;
      headers_out[i].seq = rows[i].seq;
      headers_out[i].level_offset = rows[i].level_offset;
      headers_out[i].symbol_id = rows[i].global_symbol_id;
      headers_out[i].bid_count = rows[i].bid_count;
      headers_out[i].ask_count = rows[i].ask_count;
      headers_out[i].event_type = rows[i].event_type;
    }
  }
  if (levels_out)
  {
    // Reader stores levels flat (bids then asks per event). The side
    // byte is reconstructed here using the corresponding header's
    // bid_count split.
    uint64_t k = 0;
    for (uint64_t i = 0; i < rows.size() && k < n_lv; ++i)
    {
      const auto& r = rows[i];
      for (uint16_t b = 0; b < r.bid_count && k < n_lv; ++b, ++k)
      {
        levels_out[k].price_raw = levels[k].price_raw;
        levels_out[k].qty_raw = levels[k].qty_raw;
        levels_out[k].side = 0;
      }
      for (uint16_t a = 0; a < r.ask_count && k < n_lv; ++a, ++k)
      {
        levels_out[k].price_raw = levels[k].price_raw;
        levels_out[k].qty_raw = levels[k].qty_raw;
        levels_out[k].side = 1;
      }
    }
  }
  return n_ev;
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

// FloxRiskManagerImpl — non-owning callback bundle. Outlives any runner /
// engine it's attached to (caller manages destruction). Thread-safe to
// invoke from multiple consumer threads concurrently as long as the user-
// supplied callback is itself thread-safe.
struct FloxRiskManagerImpl
{
  FloxRiskManagerCallbacks cb;
};

// Same shape, different intent. See header docs for the evaluation order.
struct FloxKillSwitchImpl
{
  FloxKillSwitchCallbacks cb;
};

struct FloxOrderValidatorImpl
{
  FloxOrderValidatorCallbacks cb;
};

// Post-emission observers. Fire after the user on_signal callback.
// Never block; return type is void.
struct FloxPnLTrackerImpl
{
  FloxPnLTrackerCallbacks cb;
};

struct FloxStorageSinkImpl
{
  FloxStorageSinkCallbacks cb;
};

struct FloxMarketDataRecorderImpl
{
  FloxMarketDataRecorderCallbacks cb;
};

struct FloxReplaySourceImpl
{
  FloxReplaySourceCallbacks cb;
};

// Pack a flox::Order into the ABI-stable FloxOrder for binding callbacks.
inline FloxOrder packOrder(const flox::Order& o) noexcept
{
  FloxOrder fo{};
  fo.id = o.id;
  fo.client_order_id = o.clientOrderId;
  fo.symbol = o.symbol;
  fo.strategy_id = o.strategyId;
  fo.order_tag = o.orderTag;
  fo.side = (o.side == flox::Side::BUY) ? 0u : 1u;
  fo.type = static_cast<uint8_t>(o.type);
  fo.time_in_force = static_cast<uint8_t>(o.timeInForce);
  // Pack ExecutionFlags bit-by-bit for ABI stability.
  uint8_t flags = 0;
  flags |= (o.flags.reduceOnly ? 0x01 : 0);
  flags |= (o.flags.closePosition ? 0x02 : 0);
  flags |= (o.flags.postOnly ? 0x04 : 0);
  flags |= static_cast<uint8_t>((o.flags.holdSide & 0x03) << 3);
  fo.flags = flags;
  fo.price_raw = o.price.raw();
  fo.quantity_raw = o.quantity.raw();
  fo.filled_quantity_raw = o.filledQuantity.raw();
  fo.trigger_price_raw = o.triggerPrice.raw();
  fo.trailing_offset_raw = o.trailingOffset.raw();
  fo.created_at_ns = o.createdAt.time_since_epoch().count();
  fo.exchange_ts_ns = o.exchangeTimestamp.has_value()
                          ? o.exchangeTimestamp->time_since_epoch().count()
                          : 0;
  return fo;
}

struct FloxExecutionListenerImpl
{
  FloxExecutionListenerCallbacks cb;
};

// Adapter that exposes a binding's FloxExecutionListenerCallbacks as a
// flox::IOrderExecutionListener — pluggable into BacktestRunner.
class CapiExecutionListener : public flox::IOrderExecutionListener
{
 public:
  CapiExecutionListener(flox::SubscriberId id, FloxExecutionListenerImpl* impl)
      : flox::IOrderExecutionListener(id), _impl(impl)
  {
  }

  void onOrderSubmitted(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_submitted)
    {
      auto fo = packOrder(o);
      _impl->cb.on_submitted(_impl->cb.user_data, &fo);
    }
  }
  void onOrderAccepted(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_accepted)
    {
      auto fo = packOrder(o);
      _impl->cb.on_accepted(_impl->cb.user_data, &fo);
    }
  }
  void onOrderPartiallyFilled(const flox::Order& o, flox::Quantity q) override
  {
    if (_impl && _impl->cb.on_partially_filled)
    {
      auto fo = packOrder(o);
      _impl->cb.on_partially_filled(_impl->cb.user_data, &fo, q.raw());
    }
  }
  void onOrderFilled(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_filled)
    {
      auto fo = packOrder(o);
      _impl->cb.on_filled(_impl->cb.user_data, &fo);
    }
  }
  void onOrderPendingCancel(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_pending_cancel)
    {
      auto fo = packOrder(o);
      _impl->cb.on_pending_cancel(_impl->cb.user_data, &fo);
    }
  }
  void onOrderCanceled(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_canceled)
    {
      auto fo = packOrder(o);
      _impl->cb.on_canceled(_impl->cb.user_data, &fo);
    }
  }
  void onOrderExpired(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_expired)
    {
      auto fo = packOrder(o);
      _impl->cb.on_expired(_impl->cb.user_data, &fo);
    }
  }
  void onOrderRejected(const flox::Order& o, const std::string& reason) override
  {
    if (_impl && _impl->cb.on_rejected)
    {
      auto fo = packOrder(o);
      _impl->cb.on_rejected(_impl->cb.user_data, &fo, reason.c_str());
    }
  }
  void onOrderReplaced(const flox::Order& oldOrder, const flox::Order& newOrder) override
  {
    if (_impl && _impl->cb.on_replaced)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replaced(_impl->cb.user_data, &fo_old, &fo_new);
    }
  }
  void onOrderPendingTrigger(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_pending_trigger)
    {
      auto fo = packOrder(o);
      _impl->cb.on_pending_trigger(_impl->cb.user_data, &fo);
    }
  }
  void onOrderTriggered(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_triggered)
    {
      auto fo = packOrder(o);
      _impl->cb.on_triggered(_impl->cb.user_data, &fo);
    }
  }
  void onTrailingStopUpdated(const flox::Order& o, flox::Price newTrigger) override
  {
    if (_impl && _impl->cb.on_trailing_stop_updated)
    {
      auto fo = packOrder(o);
      _impl->cb.on_trailing_stop_updated(_impl->cb.user_data, &fo, newTrigger.raw());
    }
  }
  void onOrderQueuePositionChange(const flox::Order& o, flox::Quantity queueAhead,
                                  flox::Quantity queueTotal) override
  {
    if (_impl && _impl->cb.on_queue_position_change)
    {
      auto fo = packOrder(o);
      _impl->cb.on_queue_position_change(_impl->cb.user_data, &fo, queueAhead.raw(),
                                         queueTotal.raw());
    }
  }
  void onOrderMarketPositionChange(const flox::Order& o, uint8_t position,
                                   int32_t distanceToBestTicks) override
  {
    if (_impl && _impl->cb.on_market_position_change)
    {
      auto fo = packOrder(o);
      _impl->cb.on_market_position_change(_impl->cb.user_data, &fo, position,
                                          distanceToBestTicks);
    }
  }
  void onOrderReplaceSubmitted(const flox::Order& oldOrder,
                               const flox::Order& newOrder) override
  {
    if (_impl && _impl->cb.on_replace_submitted)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replace_submitted(_impl->cb.user_data, &fo_old, &fo_new);
    }
  }
  void onOrderReplaceAccepted(const flox::Order& oldOrder,
                              const flox::Order& newOrder) override
  {
    if (_impl && _impl->cb.on_replace_accepted)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replace_accepted(_impl->cb.user_data, &fo_old, &fo_new);
    }
  }
  void onOrderReplaceRejected(const flox::Order& oldOrder, const flox::Order& newOrder,
                              const std::string& reason) override
  {
    if (_impl && _impl->cb.on_replace_rejected)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replace_rejected(_impl->cb.user_data, &fo_old, &fo_new,
                                    reason.c_str());
    }
  }

 private:
  FloxExecutionListenerImpl* _impl;
};

// Unpack an ABI-stable FloxOrder back into a flox::Order. Used by the
// CapiExecutor adapter when reconstructing orders for the binding's
// submit / replace / OCO callbacks (engine-side already constructs an
// Order from a Signal; we round-trip it through FloxOrder for the C ABI).
inline flox::Order unpackOrder(const FloxOrder& fo) noexcept
{
  flox::Order o{};
  o.id = fo.id;
  o.clientOrderId = fo.client_order_id;
  o.symbol = fo.symbol;
  o.strategyId = fo.strategy_id;
  o.orderTag = fo.order_tag;
  o.side = (fo.side == 0) ? flox::Side::BUY : flox::Side::SELL;
  o.type = static_cast<flox::OrderType>(fo.type);
  o.timeInForce = static_cast<flox::TimeInForce>(fo.time_in_force);
  o.flags.reduceOnly = (fo.flags & 0x01) ? 1 : 0;
  o.flags.closePosition = (fo.flags & 0x02) ? 1 : 0;
  o.flags.postOnly = (fo.flags & 0x04) ? 1 : 0;
  o.flags.holdSide = static_cast<uint8_t>((fo.flags >> 3) & 0x03);
  o.price = flox::Price::fromRaw(fo.price_raw);
  o.quantity = flox::Quantity::fromRaw(fo.quantity_raw);
  o.filledQuantity = flox::Quantity::fromRaw(fo.filled_quantity_raw);
  o.triggerPrice = flox::Price::fromRaw(fo.trigger_price_raw);
  o.trailingOffset = flox::Price::fromRaw(fo.trailing_offset_raw);
  o.createdAt = flox::TimePoint{std::chrono::nanoseconds{fo.created_at_ns}};
  if (fo.exchange_ts_ns != 0)
  {
    o.exchangeTimestamp = flox::TimePoint{std::chrono::nanoseconds{fo.exchange_ts_ns}};
  }
  return o;
}

struct FloxExecutorImpl
{
  FloxExecutorCallbacks cb;
};

// Adapter that exposes a binding's FloxExecutorCallbacks as a
// flox::IOrderExecutor — pluggable into BacktestRunner via setExecutor()
// and into FloxLiveEngineImpl's executor slot.
class CapiExecutor : public flox::IOrderExecutor
{
 public:
  explicit CapiExecutor(FloxExecutorImpl* impl) : _impl(impl) {}

  void start() override
  {
    if (_impl && _impl->cb.on_start)
    {
      _impl->cb.on_start(_impl->cb.user_data);
    }
  }
  void stop() override
  {
    if (_impl && _impl->cb.on_stop)
    {
      _impl->cb.on_stop(_impl->cb.user_data);
    }
  }

  void submitOrder(const flox::Order& o) override
  {
    if (_impl && _impl->cb.submit)
    {
      auto fo = packOrder(o);
      _impl->cb.submit(_impl->cb.user_data, &fo);
    }
  }
  void cancelOrder(flox::OrderId orderId) override
  {
    if (_impl && _impl->cb.cancel)
    {
      _impl->cb.cancel(_impl->cb.user_data, static_cast<uint64_t>(orderId));
    }
  }
  void cancelAllOrders(flox::SymbolId symbol) override
  {
    if (_impl && _impl->cb.cancel_all)
    {
      _impl->cb.cancel_all(_impl->cb.user_data, static_cast<uint32_t>(symbol));
    }
  }
  void replaceOrder(flox::OrderId oldId, const flox::Order& newOrder) override
  {
    if (_impl && _impl->cb.replace)
    {
      auto fo = packOrder(newOrder);
      _impl->cb.replace(_impl->cb.user_data, static_cast<uint64_t>(oldId), &fo);
    }
  }
  void submitOCO(const flox::OCOParams& params) override
  {
    if (_impl && _impl->cb.submit_oco)
    {
      auto fo1 = packOrder(params.order1);
      auto fo2 = packOrder(params.order2);
      _impl->cb.submit_oco(_impl->cb.user_data, &fo1, &fo2);
    }
  }
  flox::ExchangeCapabilities capabilities() const override
  {
    if (_impl == nullptr || _impl->cb.capabilities == nullptr)
    {
      return flox::ExchangeCapabilities{};
    }
    FloxExchangeCapabilities cc{};
    _impl->cb.capabilities(_impl->cb.user_data, &cc);
    flox::ExchangeCapabilities out{};
    out.supportsStopMarket = (cc.supports_stop_market != 0);
    out.supportsStopLimit = (cc.supports_stop_limit != 0);
    out.supportsTakeProfitMarket = (cc.supports_take_profit_market != 0);
    out.supportsTakeProfitLimit = (cc.supports_take_profit_limit != 0);
    out.supportsTrailingStop = (cc.supports_trailing_stop != 0);
    out.supportsIceberg = (cc.supports_iceberg != 0);
    out.supportsOCO = (cc.supports_oco != 0);
    out.supportsGTC = (cc.supports_gtc != 0);
    out.supportsIOC = (cc.supports_ioc != 0);
    out.supportsFOK = (cc.supports_fok != 0);
    out.supportsGTD = (cc.supports_gtd != 0);
    out.supportsPostOnly = (cc.supports_post_only != 0);
    out.supportsReduceOnly = (cc.supports_reduce_only != 0);
    out.supportsClosePosition = (cc.supports_close_position != 0);
    return out;
  }

 private:
  FloxExecutorImpl* _impl;
};

// Adapter that exposes a binding's FloxReplaySourceCallbacks as a
// flox::replay::IMultiSegmentReader so BacktestRunner can drive it via
// the existing forEach contract.
class CapiReplaySourceReader : public flox::replay::IMultiSegmentReader
{
 public:
  explicit CapiReplaySourceReader(FloxReplaySourceImpl* source) : _source(source) {}

  uint64_t forEach(EventCallback callback) override
  {
    if (_source == nullptr || _source->cb.next == nullptr)
    {
      return 0;
    }
    uint64_t count = 0;
    FloxReplayEvent ev{};
    while (_source->cb.next(_source->cb.user_data, &ev) != 0)
    {
      flox::replay::ReplayEvent re;
      re.timestamp_ns = ev.timestamp_ns;
      switch (ev.type)
      {
        case 1:  // Trade
        {
          re.type = flox::replay::EventType::Trade;
          re.trade.exchange_ts_ns = ev.timestamp_ns;
          re.trade.recv_ts_ns = ev.timestamp_ns;
          re.trade.price_raw = ev.trade_price_raw;
          re.trade.qty_raw = ev.trade_quantity_raw;
          re.trade.symbol_id = ev.trade_symbol;
          re.trade.side = ev.trade_is_buy ? 0u : 1u;
          break;
        }
        case 2:  // BookSnapshot
        case 3:  // BookDelta
        {
          re.type = (ev.type == 2) ? flox::replay::EventType::BookSnapshot
                                   : flox::replay::EventType::BookDelta;
          re.book_header.exchange_ts_ns = ev.timestamp_ns;
          re.book_header.recv_ts_ns = ev.timestamp_ns;
          re.book_header.symbol_id = ev.book_symbol;
          re.book_header.bid_count = static_cast<uint16_t>(ev.n_bids);
          re.book_header.ask_count = static_cast<uint16_t>(ev.n_asks);
          re.book_header.type = static_cast<uint8_t>(re.type);
          re.bids.clear();
          re.asks.clear();
          re.bids.reserve(ev.n_bids);
          re.asks.reserve(ev.n_asks);
          for (uint32_t i = 0; i < ev.n_bids; ++i)
          {
            flox::replay::BookLevel lvl{};
            lvl.price_raw = ev.bids[i].price_raw;
            lvl.qty_raw = ev.bids[i].quantity_raw;
            re.bids.push_back(lvl);
          }
          for (uint32_t i = 0; i < ev.n_asks; ++i)
          {
            flox::replay::BookLevel lvl{};
            lvl.price_raw = ev.asks[i].price_raw;
            lvl.qty_raw = ev.asks[i].quantity_raw;
            re.asks.push_back(lvl);
          }
          break;
        }
        default:
          // Unknown event type — skip without aborting playback.
          continue;
      }
      ++count;
      if (!callback(re))
      {
        break;
      }
    }
    return count;
  }

  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback) override
  {
    if (_source != nullptr && _source->cb.seek_to != nullptr)
    {
      _source->cb.seek_to(_source->cb.user_data, start_ts_ns);
    }
    return forEach(std::move(callback));
  }

  const std::vector<flox::replay::SegmentInfo>& segments() const override
  {
    return _empty_segments;
  }
  uint64_t totalEvents() const override { return 0; }

 private:
  FloxReplaySourceImpl* _source;
  std::vector<flox::replay::SegmentInfo> _empty_segments;
};

// Build a FloxOrder from a Signal for the binding-side executor hook.
// Maps SignalType → OrderType for the order types that turn into orders;
// flow-control signals (Cancel/CancelAll/Modify/OCO) don't go through
// this path because they call cancel/replace/submit_oco on the executor.
inline FloxOrder signalToFloxOrder(const Signal& sig) noexcept
{
  FloxOrder fo{};
  fo.id = sig.orderId;
  fo.symbol = sig.symbol;
  fo.side = (sig.side == Side::BUY) ? 0 : 1;
  fo.time_in_force = static_cast<uint8_t>(sig.timeInForce);
  uint8_t flags = 0;
  flags |= (sig.reduceOnly ? 0x01 : 0);
  flags |= (sig.postOnly ? 0x04 : 0);
  fo.flags = flags;
  fo.price_raw = sig.price.raw();
  fo.quantity_raw = sig.quantity.raw();
  fo.trigger_price_raw = sig.triggerPrice.raw();
  fo.trailing_offset_raw = sig.trailingOffset.raw();

  switch (sig.type)
  {
    case SignalType::Market:
      fo.type = static_cast<uint8_t>(OrderType::MARKET);
      break;
    case SignalType::Limit:
      fo.type = static_cast<uint8_t>(OrderType::LIMIT);
      break;
    case SignalType::StopMarket:
      fo.type = static_cast<uint8_t>(OrderType::STOP_MARKET);
      break;
    case SignalType::StopLimit:
      fo.type = static_cast<uint8_t>(OrderType::STOP_LIMIT);
      break;
    case SignalType::TakeProfitMarket:
      fo.type = static_cast<uint8_t>(OrderType::TAKE_PROFIT_MARKET);
      break;
    case SignalType::TakeProfitLimit:
      fo.type = static_cast<uint8_t>(OrderType::TAKE_PROFIT_LIMIT);
      break;
    case SignalType::TrailingStop:
      fo.type = static_cast<uint8_t>(OrderType::TRAILING_STOP);
      break;
    default:
      fo.type = static_cast<uint8_t>(OrderType::MARKET);
      break;
  }
  return fo;
}

class RunnerSignalHandler : public ISignalHandler
{
 public:
  RunnerSignalHandler(FloxOnSignalCallback cb, void* ud) : _cb(cb), _ud(ud) {}

  // Set or clear the optional pre-trade hooks. Atomic acquire/release swap;
  // safe to call from any thread while consumer threads are active.
  void setRiskManager(FloxRiskManagerImpl* rm) noexcept
  {
    _risk.store(rm, std::memory_order_release);
  }
  void setKillSwitch(FloxKillSwitchImpl* ks) noexcept
  {
    _kill.store(ks, std::memory_order_release);
  }
  void setOrderValidator(FloxOrderValidatorImpl* ov) noexcept
  {
    _validator.store(ov, std::memory_order_release);
  }
  void setPnLTracker(FloxPnLTrackerImpl* p) noexcept
  {
    _pnl.store(p, std::memory_order_release);
  }
  void setStorageSink(FloxStorageSinkImpl* s) noexcept
  {
    _sink.store(s, std::memory_order_release);
  }
  void setExecutor(FloxExecutorImpl* e) noexcept
  {
    _executor.store(e, std::memory_order_release);
  }
  void setTraceRecorder(void* rec) noexcept
  {
    _traceRecorder.store(rec, std::memory_order_release);
  }
  void* traceRecorder() const noexcept
  {
    return _traceRecorder.load(std::memory_order_acquire);
  }
  void setTraceFeedTsNs(int64_t ts) noexcept
  {
    _traceFeedTsNs.store(ts, std::memory_order_relaxed);
  }
  int64_t traceFeedTsNs() const noexcept
  {
    return _traceFeedTsNs.load(std::memory_order_relaxed);
  }

  void onSignal(const Signal& sig) override
  {
    // Auto-capture into the attached TraceRecorder, if any. Done before
    // the user callback so the recorded signal is in flight even if the
    // user mutates state in their callback.
    if (auto* rec = _traceRecorder.load(std::memory_order_acquire))
    {
      auto* recorder = static_cast<flox::run::TraceRecorder*>(rec);
      flox::run::SignalView view;
      view.run_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      view.feed_ts_ns = _traceFeedTsNs.load(std::memory_order_relaxed);
      view.signal_id = static_cast<uint32_t>(sig.orderId);
      view.flags = 0;
      switch (sig.type)
      {
        case SignalType::Market:
        case SignalType::Limit:
        case SignalType::StopMarket:
        case SignalType::StopLimit:
        case SignalType::TakeProfitMarket:
        case SignalType::TakeProfitLimit:
        case SignalType::TrailingStop:
          view.flags = static_cast<uint32_t>(flox::run::SignalFlags::Enter);
          break;
        case SignalType::Cancel:
        case SignalType::CancelAll:
          view.flags = static_cast<uint32_t>(flox::run::SignalFlags::Exit);
          break;
        default:
          break;
      }
      view.strength_raw = 0;
      view.symbol_ids = {static_cast<uint32_t>(sig.symbol)};
      const char* name = "unknown";
      switch (sig.type)
      {
        case SignalType::Market:
          name = "market";
          break;
        case SignalType::Limit:
          name = "limit";
          break;
        case SignalType::Cancel:
          name = "cancel";
          break;
        case SignalType::CancelAll:
          name = "cancel_all";
          break;
        case SignalType::Modify:
          name = "modify";
          break;
        case SignalType::StopMarket:
          name = "stop_market";
          break;
        case SignalType::StopLimit:
          name = "stop_limit";
          break;
        case SignalType::TakeProfitMarket:
          name = "take_profit_market";
          break;
        case SignalType::TakeProfitLimit:
          name = "take_profit_limit";
          break;
        case SignalType::TrailingStop:
          name = "trailing_stop";
          break;
        default:
          break;
      }
      view.name = name;
      recorder->writeSignal(view);
    }

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

    // Pre-trade gates, evaluated in order: KillSwitch → OrderValidator →
    // RiskManager. Each is optional; an unset hook or a NULL fn pointer
    // is a no-op (let the signal through). Returning 0 drops the signal
    // and skips the remaining hooks.
    if (auto* ks = _kill.load(std::memory_order_acquire);
        ks != nullptr && ks->cb.check != nullptr)
    {
      if (ks->cb.check(ks->cb.user_data, &fs) == 0)
      {
        return;
      }
    }
    if (auto* ov = _validator.load(std::memory_order_acquire);
        ov != nullptr && ov->cb.validate != nullptr)
    {
      if (ov->cb.validate(ov->cb.user_data, &fs) == 0)
      {
        return;
      }
    }
    if (auto* rm = _risk.load(std::memory_order_acquire);
        rm != nullptr && rm->cb.allow != nullptr)
    {
      if (rm->cb.allow(rm->cb.user_data, &fs) == 0)
      {
        return;
      }
    }

    _cb(_ud, &fs);

    // Binding-supplied executor — alternative path for order routing.
    // Runs after the user on_signal so existing on_signal-based wiring
    // (where the user submits orders directly) keeps working unchanged.
    // If a user has both an executor and on_signal-based submission, the
    // order will be sent twice — that's the user's responsibility.
    if (auto* exec = _executor.load(std::memory_order_acquire);
        exec != nullptr)
    {
      switch (sig.type)
      {
        case SignalType::Market:
        case SignalType::Limit:
        case SignalType::StopMarket:
        case SignalType::StopLimit:
        case SignalType::TakeProfitMarket:
        case SignalType::TakeProfitLimit:
        case SignalType::TrailingStop:
          if (exec->cb.submit != nullptr)
          {
            FloxOrder fo = signalToFloxOrder(sig);
            exec->cb.submit(exec->cb.user_data, &fo);
          }
          break;
        case SignalType::Cancel:
          if (exec->cb.cancel != nullptr)
          {
            exec->cb.cancel(exec->cb.user_data, sig.orderId);
          }
          break;
        case SignalType::CancelAll:
          if (exec->cb.cancel_all != nullptr)
          {
            exec->cb.cancel_all(exec->cb.user_data, sig.symbol);
          }
          break;
        case SignalType::Modify:
          if (exec->cb.replace != nullptr)
          {
            FloxOrder fo{};
            fo.id = sig.orderId;
            fo.symbol = sig.symbol;
            fo.side = (sig.side == Side::BUY) ? 0 : 1;
            fo.type = static_cast<uint8_t>(OrderType::LIMIT);
            fo.price_raw = sig.newPrice.raw();
            fo.quantity_raw = sig.newQuantity.raw();
            exec->cb.replace(exec->cb.user_data, sig.orderId, &fo);
          }
          break;
        case SignalType::OCO:
          if (exec->cb.submit_oco != nullptr)
          {
            FloxOrder fo1 = signalToFloxOrder(sig);
            fo1.type = static_cast<uint8_t>(OrderType::LIMIT);
            FloxOrder fo2 = fo1;
            fo2.price_raw = sig.triggerPrice.raw();
            exec->cb.submit_oco(exec->cb.user_data, &fo1, &fo2);
          }
          break;
      }
    }

    // Post-emission observers, fired in declared order: PnL → Storage.
    // Return type is void; observers cannot drop the signal (it's already
    // been delivered). The user callback runs first so the binding's
    // hot-path latency isn't affected by observer cost.
    if (auto* pnl = _pnl.load(std::memory_order_acquire);
        pnl != nullptr && pnl->cb.on_signal != nullptr)
    {
      pnl->cb.on_signal(pnl->cb.user_data, &fs);
    }
    if (auto* sink = _sink.load(std::memory_order_acquire);
        sink != nullptr && sink->cb.store != nullptr)
    {
      sink->cb.store(sink->cb.user_data, &fs);
    }
  }

 private:
  FloxOnSignalCallback _cb;
  void* _ud;
  std::atomic<FloxRiskManagerImpl*> _risk{nullptr};
  std::atomic<FloxKillSwitchImpl*> _kill{nullptr};
  std::atomic<FloxOrderValidatorImpl*> _validator{nullptr};
  std::atomic<FloxPnLTrackerImpl*> _pnl{nullptr};
  std::atomic<FloxStorageSinkImpl*> _sink{nullptr};
  std::atomic<FloxExecutorImpl*> _executor{nullptr};
  // Optional trace recorder. Owned by caller; written into on every
  // signal so the run captures without per-strategy instrumentation.
  std::atomic<void*> _traceRecorder{nullptr};
  std::atomic<int64_t> _traceFeedTsNs{0};
};

struct FloxRunnerImpl
{
  SymbolRegistry* registry;
  RunnerSignalHandler handler;
  std::vector<BridgeStrategy*> strategies;
  std::pmr::unsynchronized_pool_resource pool;

  // Optional market data recorder hook. Owned by caller; non-owning ptr.
  std::atomic<FloxMarketDataRecorderImpl*> recorder{nullptr};
  // Tracks whether on_start has fired without a matching on_stop, so that
  // attaching mid-run or detaching emits the right lifecycle callback.
  std::atomic<bool> recorderRunning{false};

  // Optional binding-supplied executor. Same lifecycle pattern as recorder.
  std::atomic<FloxExecutorImpl*> executor{nullptr};
  std::atomic<bool> executorRunning{false};

  FloxRunnerImpl(SymbolRegistry* reg, FloxOnSignalCallback cb, void* ud)
      : registry(reg), handler(cb, ud)
  {
  }

  void addStrategy(BridgeStrategy* s)
  {
    s->setSignalHandler(&handler);
    strategies.push_back(s);
  }

  void setRiskManager(FloxRiskManagerImpl* rm) { handler.setRiskManager(rm); }
  void setKillSwitch(FloxKillSwitchImpl* ks) { handler.setKillSwitch(ks); }
  void setOrderValidator(FloxOrderValidatorImpl* ov)
  {
    handler.setOrderValidator(ov);
  }
  void setPnLTracker(FloxPnLTrackerImpl* p) { handler.setPnLTracker(p); }
  void setStorageSink(FloxStorageSinkImpl* s) { handler.setStorageSink(s); }
  void attachTraceRecorder(void* rec) { handler.setTraceRecorder(rec); }
  void setTraceFeedTsNs(int64_t ts) { handler.setTraceFeedTsNs(ts); }
  flox::run::TraceRecorder* traceRecorder() const noexcept
  {
    return static_cast<flox::run::TraceRecorder*>(handler.traceRecorder());
  }
  int64_t traceFeedTsNs() const noexcept { return handler.traceFeedTsNs(); }

  // Attach / detach a binding-supplied executor. Lifecycle (on_start /
  // on_stop) is balanced against runner start/stop, with hot-swap
  // semantics so attach-while-running and detach fire the lifecycle
  // callbacks correctly.
  void setExecutor(FloxExecutorImpl* e)
  {
    auto* prev = executor.exchange(e, std::memory_order_acq_rel);
    handler.setExecutor(e);
    if (executorRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (e != nullptr && e->cb.on_start != nullptr)
      {
        e->cb.on_start(e->cb.user_data);
      }
    }
  }

  // Attach / detach a market data recorder. If the runner is already started
  // (recorderRunning == true), fire on_stop on the outgoing recorder and
  // on_start on the incoming one so the lifecycle stays balanced.
  void setMarketDataRecorder(FloxMarketDataRecorderImpl* r)
  {
    auto* prev = recorder.exchange(r, std::memory_order_acq_rel);
    if (recorderRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (r != nullptr && r->cb.on_start != nullptr)
      {
        r->cb.on_start(r->cb.user_data);
      }
    }
  }

  void start()
  {
    for (auto* s : strategies)
    {
      s->start();
    }
    recorderRunning.store(true, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_start != nullptr)
    {
      r->cb.on_start(r->cb.user_data);
    }
    executorRunning.store(true, std::memory_order_release);
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_start != nullptr)
    {
      e->cb.on_start(e->cb.user_data);
    }
  }

  void stop()
  {
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_stop != nullptr)
    {
      e->cb.on_stop(e->cb.user_data);
    }
    executorRunning.store(false, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_stop != nullptr)
    {
      r->cb.on_stop(r->cb.user_data);
    }
    recorderRunning.store(false, std::memory_order_release);
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

    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_trade != nullptr)
    {
      FloxTradeData td{};
      td.symbol = symbol;
      td.price_raw = trade.price.raw();
      td.quantity_raw = trade.quantity.raw();
      td.is_buy = is_buy ? 1 : 0;
      td.exchange_ts_ns = ts_ns;
      r->cb.on_trade(r->cb.user_data, &td);
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

    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_book_update != nullptr)
    {
      // Re-pack levels into FloxBookLevel for the C ABI. Stack buffers up
      // to a small threshold; heap-fall back for deep books.
      constexpr uint32_t kStackLevels = 64;
      FloxBookLevel stackBids[kStackLevels];
      FloxBookLevel stackAsks[kStackLevels];
      std::vector<FloxBookLevel> heapBids;
      std::vector<FloxBookLevel> heapAsks;
      FloxBookLevel* bidPtr = stackBids;
      FloxBookLevel* askPtr = stackAsks;
      if (n_bids > kStackLevels)
      {
        heapBids.resize(n_bids);
        bidPtr = heapBids.data();
      }
      if (n_asks > kStackLevels)
      {
        heapAsks.resize(n_asks);
        askPtr = heapAsks.data();
      }
      for (uint32_t i = 0; i < n_bids; ++i)
      {
        bidPtr[i].price_raw = ev.update.bids[i].price.raw();
        bidPtr[i].quantity_raw = ev.update.bids[i].quantity.raw();
      }
      for (uint32_t i = 0; i < n_asks; ++i)
      {
        askPtr[i].price_raw = ev.update.asks[i].price.raw();
        askPtr[i].quantity_raw = ev.update.asks[i].quantity.raw();
      }
      r->cb.on_book_update(r->cb.user_data, symbol, /*is_snapshot=*/1u,
                           bidPtr, n_bids, askPtr, n_asks, ts_ns);
    }
  }

  void onBar(uint32_t symbol, uint8_t bar_type, uint64_t bar_type_param,
             double open, double high, double low, double close,
             double volume, double buy_volume,
             int64_t start_time_ns, int64_t end_time_ns,
             uint8_t close_reason)
  {
    BarEvent ev{};
    ev.symbol = symbol;
    ev.barType = static_cast<BarType>(bar_type);
    ev.barTypeParam = bar_type_param;
    ev.bar.open = Price::fromDouble(open);
    ev.bar.high = Price::fromDouble(high);
    ev.bar.low = Price::fromDouble(low);
    ev.bar.close = Price::fromDouble(close);
    ev.bar.volume = Volume::fromDouble(volume);
    ev.bar.buyVolume = Volume::fromDouble(buy_volume);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{start_time_ns}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{end_time_ns}};
    ev.bar.reason = static_cast<BarCloseReason>(close_reason);

    for (auto* s : strategies)
    {
      s->onBar(ev);
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
  std::unique_ptr<BarBus> barBus;
  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> bookPool;

  // Per-strategy signal handlers (owned here, outlive strategies)
  std::vector<std::unique_ptr<RunnerSignalHandler>> handlers;
  std::vector<BridgeStrategy*> strategies;

  // Latest pre-trade hooks attached to this engine. Stored here (rather
  // than only on the handlers) so newly-added strategies inherit the
  // existing setting. Caller owns the *Impl objects; we hold non-owning ptrs.
  std::atomic<FloxRiskManagerImpl*> riskManager{nullptr};
  std::atomic<FloxKillSwitchImpl*> killSwitch{nullptr};
  std::atomic<FloxOrderValidatorImpl*> orderValidator{nullptr};
  std::atomic<FloxPnLTrackerImpl*> pnlTracker{nullptr};
  std::atomic<FloxStorageSinkImpl*> storageSink{nullptr};
  std::atomic<FloxMarketDataRecorderImpl*> recorder{nullptr};
  std::atomic<bool> recorderRunning{false};
  std::atomic<FloxExecutorImpl*> executor{nullptr};
  std::atomic<bool> executorRunning{false};

  explicit FloxLiveEngineImpl(SymbolRegistry* reg)
      : registry(reg),
        tradeBus(std::make_unique<TradeBus>()),
        bookBus(std::make_unique<BookUpdateBus>()),
        barBus(std::make_unique<BarBus>())
  {
  }

  void addStrategy(BridgeStrategy* s, FloxOnSignalCallback cb, void* ud)
  {
    auto h = std::make_unique<RunnerSignalHandler>(cb, ud);
    h->setRiskManager(riskManager.load(std::memory_order_acquire));
    h->setKillSwitch(killSwitch.load(std::memory_order_acquire));
    h->setOrderValidator(orderValidator.load(std::memory_order_acquire));
    h->setPnLTracker(pnlTracker.load(std::memory_order_acquire));
    h->setStorageSink(storageSink.load(std::memory_order_acquire));
    h->setExecutor(executor.load(std::memory_order_acquire));
    s->setSignalHandler(h.get());
    handlers.push_back(std::move(h));
    tradeBus->subscribe(s);
    bookBus->subscribe(s);
    barBus->subscribe(s);
    strategies.push_back(s);
  }

  // Update the pre-trade hooks on every existing handler and remember the
  // setting so subsequently-added strategies inherit it.
  void setRiskManager(FloxRiskManagerImpl* rm)
  {
    riskManager.store(rm, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setRiskManager(rm);
    }
  }
  void setKillSwitch(FloxKillSwitchImpl* ks)
  {
    killSwitch.store(ks, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setKillSwitch(ks);
    }
  }
  void setOrderValidator(FloxOrderValidatorImpl* ov)
  {
    orderValidator.store(ov, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setOrderValidator(ov);
    }
  }
  void setPnLTracker(FloxPnLTrackerImpl* p)
  {
    pnlTracker.store(p, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setPnLTracker(p);
    }
  }
  void setStorageSink(FloxStorageSinkImpl* s)
  {
    storageSink.store(s, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setStorageSink(s);
    }
  }

  // Attach / detach a market data recorder. Lifecycle (on_start/on_stop)
  // is balanced against engine.start()/stop(), mirroring runner semantics.
  void setMarketDataRecorder(FloxMarketDataRecorderImpl* r)
  {
    auto* prev = recorder.exchange(r, std::memory_order_acq_rel);
    if (recorderRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (r != nullptr && r->cb.on_start != nullptr)
      {
        r->cb.on_start(r->cb.user_data);
      }
    }
  }

  // Attach / detach a binding-supplied executor. Updates every existing
  // signal handler (each strategy has its own) and remembers the setting
  // so subsequently-added strategies inherit it. Lifecycle balanced
  // against engine.start()/stop().
  void setExecutor(FloxExecutorImpl* e)
  {
    auto* prev = executor.exchange(e, std::memory_order_acq_rel);
    for (auto& h : handlers)
    {
      h->setExecutor(e);
    }
    if (executorRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (e != nullptr && e->cb.on_start != nullptr)
      {
        e->cb.on_start(e->cb.user_data);
      }
    }
  }

  void start()
  {
    tradeBus->start();
    bookBus->start();
    barBus->start();
    for (auto* s : strategies)
    {
      s->start();
    }
    recorderRunning.store(true, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_start != nullptr)
    {
      r->cb.on_start(r->cb.user_data);
    }
    executorRunning.store(true, std::memory_order_release);
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_start != nullptr)
    {
      e->cb.on_start(e->cb.user_data);
    }
  }

  void stop()
  {
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_stop != nullptr)
    {
      e->cb.on_stop(e->cb.user_data);
    }
    executorRunning.store(false, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_stop != nullptr)
    {
      r->cb.on_stop(r->cb.user_data);
    }
    recorderRunning.store(false, std::memory_order_release);
    for (auto* s : strategies)
    {
      s->stop();
    }
    tradeBus->stop();
    bookBus->stop();
    barBus->stop();
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

    // Recorder runs on the publisher thread (caller), before publish
    // becomes visible to consumer-side strategies. This mirrors "the
    // engine is being fed this event"; consumer-side timing isn't part
    // of the recorder contract.
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_trade != nullptr)
    {
      FloxTradeData td{};
      td.symbol = symbol;
      td.price_raw = ev.trade.price.raw();
      td.quantity_raw = ev.trade.quantity.raw();
      td.is_buy = is_buy ? 1 : 0;
      td.exchange_ts_ns = ts_ns;
      r->cb.on_trade(r->cb.user_data, &td);
    }
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

    // Mirror to recorder before publish — see publishTrade comment.
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_book_update != nullptr)
    {
      constexpr uint32_t kStackLevels = 64;
      FloxBookLevel stackBids[kStackLevels];
      FloxBookLevel stackAsks[kStackLevels];
      std::vector<FloxBookLevel> heapBids;
      std::vector<FloxBookLevel> heapAsks;
      FloxBookLevel* bidPtr = stackBids;
      FloxBookLevel* askPtr = stackAsks;
      if (n_bids > kStackLevels)
      {
        heapBids.resize(n_bids);
        bidPtr = heapBids.data();
      }
      if (n_asks > kStackLevels)
      {
        heapAsks.resize(n_asks);
        askPtr = heapAsks.data();
      }
      for (uint32_t i = 0; i < n_bids; ++i)
      {
        bidPtr[i].price_raw = ev->update.bids[i].price.raw();
        bidPtr[i].quantity_raw = ev->update.bids[i].quantity.raw();
      }
      for (uint32_t i = 0; i < n_asks; ++i)
      {
        askPtr[i].price_raw = ev->update.asks[i].price.raw();
        askPtr[i].quantity_raw = ev->update.asks[i].quantity.raw();
      }
      r->cb.on_book_update(r->cb.user_data, symbol, /*is_snapshot=*/1u,
                           bidPtr, n_bids, askPtr, n_asks, ts_ns);
    }

    bookBus->publish(std::move(evOpt.value()));
  }

  void publishBar(uint32_t symbol, uint8_t bar_type, uint64_t bar_type_param,
                  double open, double high, double low, double close,
                  double volume, double buy_volume,
                  int64_t start_time_ns, int64_t end_time_ns,
                  uint8_t close_reason)
  {
    BarEvent ev{};
    ev.symbol = symbol;
    ev.barType = static_cast<BarType>(bar_type);
    ev.barTypeParam = bar_type_param;
    ev.bar.open = Price::fromDouble(open);
    ev.bar.high = Price::fromDouble(high);
    ev.bar.low = Price::fromDouble(low);
    ev.bar.close = Price::fromDouble(close);
    ev.bar.volume = Volume::fromDouble(volume);
    ev.bar.buyVolume = Volume::fromDouble(buy_volume);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{start_time_ns}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{end_time_ns}};
    ev.bar.reason = static_cast<BarCloseReason>(close_reason);
    barBus->publish(ev);
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
// Adapters: wrap callback-shaped FloxRiskManagerImpl /
// FloxKillSwitchImpl / FloxOrderValidatorImpl / FloxPnLTrackerImpl
// into engine-side I* interfaces so the new BacktestRunner setters
// (which are C++-typed) accept the existing C ABI handles bindings
// already create. The live runner doesn't need this bridge — it
// gates at signal-level inside RunnerSignalHandler — but the
// engine-level BacktestRunner takes Order-shaped callbacks, so we
// build a FloxSignal from the Order and forward.
// ──────────────────────────────────────────────────────────────

inline FloxSignal orderToFloxSignal(const Order& order) noexcept
{
  FloxSignal fs{};
  fs.order_id = order.id;
  fs.symbol = order.symbol;
  fs.side = (order.side == Side::BUY) ? 0 : 1;
  fs.price = order.price.toDouble();
  fs.quantity = order.quantity.toDouble();
  fs.order_type = static_cast<uint8_t>(order.type);
  return fs;
}

class CapiBacktestRiskManager : public flox::IRiskManager
{
 public:
  explicit CapiBacktestRiskManager(FloxRiskManagerImpl* impl) : _impl(impl) {}

  bool allow(const Order& order) const override
  {
    if (_impl == nullptr || _impl->cb.allow == nullptr)
    {
      return true;
    }
    FloxSignal fs = orderToFloxSignal(order);
    return _impl->cb.allow(_impl->cb.user_data, &fs) != 0;
  }

 private:
  FloxRiskManagerImpl* _impl;
};

class CapiBacktestKillSwitch : public flox::IKillSwitch
{
 public:
  explicit CapiBacktestKillSwitch(FloxKillSwitchImpl* impl) : _impl(impl) {}

  // The C ABI kill switch is a per-signal check. The engine contract
  // is "check(order) may trigger; isTriggered() reports current state".
  // Cache the latest check result so the runner's gate logic
  // (`check(order); if (isTriggered()) drop`) maps cleanly.
  void check(const Order& order) override
  {
    if (_impl == nullptr || _impl->cb.check == nullptr)
    {
      _triggered = false;
      return;
    }
    FloxSignal fs = orderToFloxSignal(order);
    // C ABI: 0 → drop / kill-switch active.
    _triggered = (_impl->cb.check(_impl->cb.user_data, &fs) == 0);
  }
  void trigger(const std::string& reason) override
  {
    _triggered = true;
    _reason = reason;
  }
  bool isTriggered() const override { return _triggered; }
  std::string reason() const override { return _reason; }

 private:
  FloxKillSwitchImpl* _impl;
  bool _triggered{false};
  std::string _reason;
};

class CapiBacktestOrderValidator : public flox::IOrderValidator
{
 public:
  explicit CapiBacktestOrderValidator(FloxOrderValidatorImpl* impl) : _impl(impl) {}

  // The C ABI validator returns 0/1 without a reason string. Engine
  // contract returns a `reason` parameter — leave it empty when the
  // C ABI rejects so the rejected event still surfaces upward; the
  // user-visible reason can be plumbed through the binding's own
  // PyOrderValidator if richer messaging is needed.
  bool validate(const Order& order, std::string& /*reason*/) const override
  {
    if (_impl == nullptr || _impl->cb.validate == nullptr)
    {
      return true;
    }
    FloxSignal fs = orderToFloxSignal(order);
    return _impl->cb.validate(_impl->cb.user_data, &fs) != 0;
  }

 private:
  FloxOrderValidatorImpl* _impl;
};

class CapiBacktestPnLTracker : public flox::IPnLTracker
{
 public:
  explicit CapiBacktestPnLTracker(FloxPnLTrackerImpl* impl) : _impl(impl) {}

  // Engine fires onOrderFilled per fill; C ABI signature takes a
  // FloxSignal. Convert and forward — bindings that want richer fill
  // detail should attach an ExecutionListener instead.
  void onOrderFilled(const Order& order) override
  {
    if (_impl == nullptr || _impl->cb.on_signal == nullptr)
    {
      return;
    }
    FloxSignal fs = orderToFloxSignal(order);
    _impl->cb.on_signal(_impl->cb.user_data, &fs);
  }

 private:
  FloxPnLTrackerImpl* _impl;
};

// ──────────────────────────────────────────────────────────────
// FloxBacktestRunnerImpl
// ──────────────────────────────────────────────────────────────

struct FloxBacktestRunnerImpl
{
  SymbolRegistry* registry;
  std::unique_ptr<BacktestRunner> runner;
  // Adapters created when bindings register execution listeners. Owned
  // here so they outlive the listener registration on the runner.
  std::vector<std::unique_ptr<CapiExecutionListener>> executionListenerAdapters;
  // Adapter for the binding-supplied executor. Owned so it outlives the
  // runner's non-owning pointer. Replaced on every set call.
  std::unique_ptr<CapiExecutor> executorAdapter;
  // Adapters for the four pre-trade gate hooks. The runner stores
  // raw IRiskManager / IKillSwitch / etc pointers; we own the
  // adapters so they outlive the runner's non-owning references.
  // Replaced on every set call (NULL detaches).
  std::unique_ptr<CapiBacktestRiskManager> riskAdapter;
  std::unique_ptr<CapiBacktestKillSwitch> killAdapter;
  std::unique_ptr<CapiBacktestOrderValidator> validatorAdapter;
  std::unique_ptr<CapiBacktestPnLTracker> pnlAdapter;
  // Most recent BacktestResult, kept after each run_* call so bindings
  // can fetch the equity curve / trades without re-running the
  // backtest. Replaced on every run.
  std::optional<BacktestResult> lastResult;

  explicit FloxBacktestRunnerImpl(SymbolRegistry* reg, double feeRate, double initialCapital)
      : registry(reg)
  {
    BacktestConfig cfg{};
    cfg.feeRate = feeRate;
    cfg.initialCapital = initialCapital;
    cfg.usePercentageFee = true;
    runner = std::make_unique<BacktestRunner>(cfg);
  }

  void addExecutionListener(FloxExecutionListenerImpl* impl)
  {
    if (impl == nullptr)
    {
      return;
    }
    auto id = static_cast<flox::SubscriberId>(executionListenerAdapters.size() + 1);
    auto adapter = std::make_unique<CapiExecutionListener>(id, impl);
    runner->addExecutionListener(adapter.get());
    executionListenerAdapters.push_back(std::move(adapter));
  }

  void setExecutor(FloxExecutorImpl* impl)
  {
    if (impl == nullptr)
    {
      runner->setExecutor(nullptr);
      executorAdapter.reset();
      return;
    }
    executorAdapter = std::make_unique<CapiExecutor>(impl);
    runner->setExecutor(executorAdapter.get());
  }

  void setStrategy(BridgeStrategy* bridge)
  {
    runner->setStrategy(bridge);
  }

  // Pre-trade gate parity with the live runner. NULL detaches.
  // The adapter owns conversion from FloxSignal-shaped callbacks
  // into the engine's Order-shaped I* interfaces.
  void setRiskManager(FloxRiskManagerImpl* rm)
  {
    if (rm == nullptr)
    {
      runner->setRiskManager(nullptr);
      riskAdapter.reset();
      return;
    }
    riskAdapter = std::make_unique<CapiBacktestRiskManager>(rm);
    runner->setRiskManager(riskAdapter.get());
  }
  void setKillSwitch(FloxKillSwitchImpl* ks)
  {
    if (ks == nullptr)
    {
      runner->setKillSwitch(nullptr);
      killAdapter.reset();
      return;
    }
    killAdapter = std::make_unique<CapiBacktestKillSwitch>(ks);
    runner->setKillSwitch(killAdapter.get());
  }
  void setOrderValidator(FloxOrderValidatorImpl* ov)
  {
    if (ov == nullptr)
    {
      runner->setOrderValidator(nullptr);
      validatorAdapter.reset();
      return;
    }
    validatorAdapter = std::make_unique<CapiBacktestOrderValidator>(ov);
    runner->setOrderValidator(validatorAdapter.get());
  }
  void setPnLTracker(FloxPnLTrackerImpl* tracker)
  {
    if (tracker == nullptr)
    {
      runner->setPnLTracker(nullptr);
      pnlAdapter.reset();
      return;
    }
    pnlAdapter = std::make_unique<CapiBacktestPnLTracker>(tracker);
    runner->setPnLTracker(pnlAdapter.get());
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
    lastResult = std::move(result);
    return 1;
  }

  int runTape(const char* tape_dir, FloxBacktestStats* out)
  {
    if (!runner)
    {
      return 0;
    }
    try
    {
      BacktestResult result = runner->runTape(std::filesystem::path(tape_dir));
      if (out)
      {
        fillStats(result.computeStats(), out);
      }
      lastResult = std::move(result);
      return 1;
    }
    catch (const std::exception&)
    {
      return 0;
    }
  }

  int runTapes(const char* const* tape_dirs, uint32_t n_dirs,
               FloxBacktestStats* out)
  {
    if (!runner || !tape_dirs || n_dirs == 0)
    {
      return 0;
    }
    try
    {
      std::vector<std::filesystem::path> paths;
      paths.reserve(n_dirs);
      for (uint32_t i = 0; i < n_dirs; ++i)
      {
        paths.emplace_back(tape_dirs[i] ? tape_dirs[i] : "");
      }
      BacktestResult result = runner->runTapes(paths);
      if (out)
      {
        fillStats(result.computeStats(), out);
      }
      lastResult = std::move(result);
      return 1;
    }
    catch (const std::exception&)
    {
      return 0;
    }
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

  int runFullBars(const int64_t* start_ns, const int64_t* end_ns,
                  const double* open, const double* high, const double* low,
                  const double* close, const double* volume, uint32_t n,
                  const char* symbol, uint8_t bar_type, uint64_t bar_type_param,
                  FloxBacktestStats* out)
  {
    uint32_t id = resolveSymbol(symbol);
    std::vector<BarEvent> events;
    events.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      BarEvent ev{};
      ev.symbol = id;
      ev.barType = static_cast<BarType>(bar_type);
      ev.barTypeParam = bar_type_param;
      ev.bar.open = Price::fromDouble(open[i]);
      ev.bar.high = Price::fromDouble(high[i]);
      ev.bar.low = Price::fromDouble(low[i]);
      ev.bar.close = Price::fromDouble(close[i]);
      ev.bar.volume = Volume::fromDouble(volume ? volume[i] : 0.0);
      ev.bar.startTime = TimePoint{std::chrono::nanoseconds{normalizeTs(start_ns[i])}};
      ev.bar.endTime = TimePoint{std::chrono::nanoseconds{normalizeTs(end_ns[i])}};
      ev.bar.reason = BarCloseReason::Threshold;
      events.push_back(ev);
    }
    BacktestResult result = runner->runBars(events);
    if (out)
    {
      fillStats(result.computeStats(), out);
    }
    lastResult = std::move(result);
    return 1;
  }

  int runReplaySource(FloxReplaySourceImpl* source, FloxBacktestStats* out)
  {
    if (source == nullptr)
    {
      return 0;
    }
    if (source->cb.on_start != nullptr)
    {
      source->cb.on_start(source->cb.user_data);
    }
    CapiReplaySourceReader reader(source);
    BacktestResult result = runner->run(reader);
    if (source->cb.on_stop != nullptr)
    {
      source->cb.on_stop(source->cb.user_data);
    }
    if (out)
    {
      fillStats(result.computeStats(), out);
    }
    lastResult = std::move(result);
    return 1;
  }
};

static FloxBacktestRunnerImpl* toBacktestRunner(FloxBacktestRunnerHandle h)
{
  return static_cast<FloxBacktestRunnerImpl*>(h);
}

}  // namespace capi_impl

using namespace capi_impl;

// ============================================================
// FloxRiskManager C API
// ============================================================

FloxRiskManagerHandle flox_risk_manager_create(FloxRiskManagerCallbacks callbacks)
{
  auto* rm = new FloxRiskManagerImpl{callbacks};
  return static_cast<FloxRiskManagerHandle>(rm);
}

FloxRiskManagerHandle flox_risk_manager_create_p(const FloxRiskManagerCallbacks* callbacks)
{
  FloxRiskManagerCallbacks cbs = callbacks ? *callbacks : FloxRiskManagerCallbacks{};
  return flox_risk_manager_create(cbs);
}

void flox_risk_manager_destroy(FloxRiskManagerHandle rm)
{
  delete static_cast<FloxRiskManagerImpl*>(rm);
}

FloxKillSwitchHandle flox_kill_switch_create(FloxKillSwitchCallbacks callbacks)
{
  auto* ks = new FloxKillSwitchImpl{callbacks};
  return static_cast<FloxKillSwitchHandle>(ks);
}

FloxKillSwitchHandle flox_kill_switch_create_p(const FloxKillSwitchCallbacks* callbacks)
{
  FloxKillSwitchCallbacks cbs = callbacks ? *callbacks : FloxKillSwitchCallbacks{};
  return flox_kill_switch_create(cbs);
}

void flox_kill_switch_destroy(FloxKillSwitchHandle ks)
{
  delete static_cast<FloxKillSwitchImpl*>(ks);
}

FloxOrderValidatorHandle flox_order_validator_create(FloxOrderValidatorCallbacks callbacks)
{
  auto* ov = new FloxOrderValidatorImpl{callbacks};
  return static_cast<FloxOrderValidatorHandle>(ov);
}

FloxOrderValidatorHandle flox_order_validator_create_p(const FloxOrderValidatorCallbacks* callbacks)
{
  FloxOrderValidatorCallbacks cbs = callbacks ? *callbacks : FloxOrderValidatorCallbacks{};
  return flox_order_validator_create(cbs);
}

void flox_order_validator_destroy(FloxOrderValidatorHandle ov)
{
  delete static_cast<FloxOrderValidatorImpl*>(ov);
}

FloxPnLTrackerHandle flox_pnl_tracker_create(FloxPnLTrackerCallbacks callbacks)
{
  return static_cast<FloxPnLTrackerHandle>(new FloxPnLTrackerImpl{callbacks});
}

FloxPnLTrackerHandle flox_pnl_tracker_create_p(const FloxPnLTrackerCallbacks* callbacks)
{
  FloxPnLTrackerCallbacks cbs = callbacks ? *callbacks : FloxPnLTrackerCallbacks{};
  return flox_pnl_tracker_create(cbs);
}

void flox_pnl_tracker_destroy(FloxPnLTrackerHandle tracker)
{
  delete static_cast<FloxPnLTrackerImpl*>(tracker);
}

FloxStorageSinkHandle flox_storage_sink_create(FloxStorageSinkCallbacks callbacks)
{
  return static_cast<FloxStorageSinkHandle>(new FloxStorageSinkImpl{callbacks});
}

FloxStorageSinkHandle flox_storage_sink_create_p(const FloxStorageSinkCallbacks* callbacks)
{
  FloxStorageSinkCallbacks cbs = callbacks ? *callbacks : FloxStorageSinkCallbacks{};
  return flox_storage_sink_create(cbs);
}

void flox_storage_sink_destroy(FloxStorageSinkHandle sink)
{
  delete static_cast<FloxStorageSinkImpl*>(sink);
}

FloxMarketDataRecorderHandle flox_market_data_recorder_create(
    FloxMarketDataRecorderCallbacks callbacks)
{
  return static_cast<FloxMarketDataRecorderHandle>(
      new FloxMarketDataRecorderImpl{callbacks});
}

FloxMarketDataRecorderHandle flox_market_data_recorder_create_p(
    const FloxMarketDataRecorderCallbacks* callbacks)
{
  FloxMarketDataRecorderCallbacks cbs = callbacks ? *callbacks : FloxMarketDataRecorderCallbacks{};
  return flox_market_data_recorder_create(cbs);
}

void flox_market_data_recorder_destroy(FloxMarketDataRecorderHandle recorder)
{
  delete static_cast<FloxMarketDataRecorderImpl*>(recorder);
}

// ── Binary-log recorder hook ──────────────────────────────────────────
namespace capi_impl
{
struct FloxBinaryLogRecorderHookImpl
{
  flox::replay::BinaryLogRecorderHook hook;
  // FloxMarketDataRecorderImpl whose callbacks bridge engine events
  // straight into hook.onTrade / onBookUpdate. user_data points at
  // `this`. Lifetime is tied to the owning impl — DO NOT separately
  // free via flox_market_data_recorder_destroy.
  FloxMarketDataRecorderImpl recorder_view{};

  explicit FloxBinaryLogRecorderHookImpl(flox::replay::BinaryLogRecorderHookConfig cfg)
      : hook(std::move(cfg))
  {
  }
};

static void blrhOnTrade(void* ud, const FloxTradeData* t)
{
  if (!t)
  {
    return;
  }
  auto* self = static_cast<FloxBinaryLogRecorderHookImpl*>(ud);
  self->hook.onTrade(t->symbol, t->price_raw, t->quantity_raw,
                     t->is_buy != 0, t->exchange_ts_ns, /*recv_ts_ns=*/0);
}

static void blrhOnBookUpdate(void* ud, uint32_t symbol, uint8_t is_snapshot,
                             const FloxBookLevel* bids, uint32_t n_bids,
                             const FloxBookLevel* asks, uint32_t n_asks,
                             int64_t exchange_ts_ns)
{
  auto* self = static_cast<FloxBinaryLogRecorderHookImpl*>(ud);
  auto* bid_levels = reinterpret_cast<const flox::replay::BookLevel*>(bids);
  auto* ask_levels = reinterpret_cast<const flox::replay::BookLevel*>(asks);
  self->hook.onBookUpdate(symbol, is_snapshot != 0, bid_levels, n_bids,
                          ask_levels, n_asks, exchange_ts_ns, /*recv_ts_ns=*/0);
}

static void blrhOnStart(void* ud)
{
  static_cast<FloxBinaryLogRecorderHookImpl*>(ud)->hook.start();
}

static void blrhOnStop(void* ud)
{
  static_cast<FloxBinaryLogRecorderHookImpl*>(ud)->hook.stop();
}
}  // namespace capi_impl

FloxBinaryLogRecorderHookHandle
flox_binary_log_recorder_hook_create(const char* output_dir,
                                     uint64_t max_segment_mb,
                                     uint8_t exchange_id,
                                     uint8_t compression)
{
  return flox_binary_log_recorder_hook_create_ex(output_dir, max_segment_mb,
                                                 exchange_id, compression,
                                                 nullptr, nullptr);
}

FloxBinaryLogRecorderHookHandle
flox_binary_log_recorder_hook_create_ex(const char* output_dir,
                                        uint64_t max_segment_mb,
                                        uint8_t exchange_id,
                                        uint8_t compression,
                                        const char* exchange_name,
                                        const char* instrument_type)
{
  flox::replay::BinaryLogRecorderHookConfig cfg{};
  cfg.output_dir = output_dir ? output_dir : "";
  cfg.max_segment_bytes = max_segment_mb * 1024ull * 1024ull;
  cfg.exchange_id = exchange_id;
  cfg.compression = static_cast<flox::replay::CompressionType>(compression);

  const bool has_exchange = exchange_name && *exchange_name;
  const bool has_instrument = instrument_type && *instrument_type;
  if (has_exchange || has_instrument)
  {
    flox::replay::RecordingMetadata meta{};
    if (has_exchange)
    {
      meta.exchange = exchange_name;
    }
    if (has_instrument)
    {
      meta.instrument_type = instrument_type;
    }
    cfg.metadata = std::move(meta);
  }

  auto* impl = new capi_impl::FloxBinaryLogRecorderHookImpl(std::move(cfg));
  impl->recorder_view.cb.on_trade = &capi_impl::blrhOnTrade;
  impl->recorder_view.cb.on_book_update = &capi_impl::blrhOnBookUpdate;
  impl->recorder_view.cb.on_start = &capi_impl::blrhOnStart;
  impl->recorder_view.cb.on_stop = &capi_impl::blrhOnStop;
  impl->recorder_view.cb.user_data = impl;
  return static_cast<FloxBinaryLogRecorderHookHandle>(impl);
}

void flox_binary_log_recorder_hook_destroy(FloxBinaryLogRecorderHookHandle h)
{
  delete static_cast<capi_impl::FloxBinaryLogRecorderHookImpl*>(h);
}

FloxMarketDataRecorderHandle
flox_binary_log_recorder_hook_as_recorder(FloxBinaryLogRecorderHookHandle h)
{
  if (!h)
  {
    return nullptr;
  }
  return static_cast<FloxMarketDataRecorderHandle>(
      &static_cast<capi_impl::FloxBinaryLogRecorderHookImpl*>(h)->recorder_view);
}

void flox_binary_log_recorder_hook_add_symbol(FloxBinaryLogRecorderHookHandle h,
                                              uint32_t symbol_id,
                                              const char* name,
                                              const char* base,
                                              const char* quote,
                                              int8_t price_precision,
                                              int8_t qty_precision)
{
  if (!h)
  {
    return;
  }
  flox::replay::SymbolInfo info;
  info.symbol_id = symbol_id;
  info.name = name ? name : "";
  info.base_asset = base ? base : "";
  info.quote_asset = quote ? quote : "";
  info.price_precision = price_precision;
  info.qty_precision = qty_precision;
  static_cast<capi_impl::FloxBinaryLogRecorderHookImpl*>(h)->hook.addSymbol(info);
}

void flox_binary_log_recorder_hook_flush(FloxBinaryLogRecorderHookHandle h)
{
  if (h)
  {
    static_cast<capi_impl::FloxBinaryLogRecorderHookImpl*>(h)->hook.flush();
  }
}

FloxWriterStats flox_binary_log_recorder_hook_stats(FloxBinaryLogRecorderHookHandle h)
{
  if (!h)
  {
    return {};
  }
  auto s = static_cast<capi_impl::FloxBinaryLogRecorderHookImpl*>(h)->hook.stats();
  return {s.bytes_written, s.trades_written + s.book_updates_written,
          s.segments_created, s.trades_written};
}

void flox_binary_log_recorder_hook_stats_p(void* h, void* out)
{
  auto s = flox_binary_log_recorder_hook_stats(static_cast<FloxBinaryLogRecorderHookHandle>(h));
  memcpy(out, &s, sizeof(s));
}

FloxReplaySourceHandle flox_replay_source_create(FloxReplaySourceCallbacks callbacks)
{
  return static_cast<FloxReplaySourceHandle>(
      new FloxReplaySourceImpl{callbacks});
}

FloxReplaySourceHandle flox_replay_source_create_p(const FloxReplaySourceCallbacks* callbacks)
{
  FloxReplaySourceCallbacks cbs = callbacks ? *callbacks : FloxReplaySourceCallbacks{};
  return flox_replay_source_create(cbs);
}

void flox_replay_source_destroy(FloxReplaySourceHandle source)
{
  delete static_cast<FloxReplaySourceImpl*>(source);
}

uint8_t flox_replay_source_seek_to(FloxReplaySourceHandle source, int64_t timestamp_ns)
{
  auto* s = static_cast<FloxReplaySourceImpl*>(source);
  if (s == nullptr || s->cb.seek_to == nullptr)
  {
    return 0;
  }
  return s->cb.seek_to(s->cb.user_data, timestamp_ns);
}

FloxExecutionListenerHandle
flox_execution_listener_create(FloxExecutionListenerCallbacks callbacks)
{
  return static_cast<FloxExecutionListenerHandle>(
      new FloxExecutionListenerImpl{callbacks});
}

FloxExecutionListenerHandle
flox_execution_listener_create_p(const FloxExecutionListenerCallbacks* callbacks)
{
  FloxExecutionListenerCallbacks cbs = callbacks ? *callbacks : FloxExecutionListenerCallbacks{};
  return flox_execution_listener_create(cbs);
}

void flox_execution_listener_destroy(FloxExecutionListenerHandle listener)
{
  delete static_cast<FloxExecutionListenerImpl*>(listener);
}

FloxExecutorHandle flox_executor_create(FloxExecutorCallbacks callbacks)
{
  return static_cast<FloxExecutorHandle>(new FloxExecutorImpl{callbacks});
}

FloxExecutorHandle flox_executor_create_p(const FloxExecutorCallbacks* callbacks)
{
  FloxExecutorCallbacks cbs = callbacks ? *callbacks : FloxExecutorCallbacks{};
  return flox_executor_create(cbs);
}

void flox_executor_destroy(FloxExecutorHandle executor)
{
  delete static_cast<FloxExecutorImpl*>(executor);
}

void flox_executor_get_capabilities(FloxExecutorHandle executor,
                                    FloxExchangeCapabilities* caps_out)
{
  if (caps_out == nullptr)
  {
    return;
  }
  *caps_out = FloxExchangeCapabilities{};
  auto* impl = static_cast<FloxExecutorImpl*>(executor);
  if (impl == nullptr || impl->cb.capabilities == nullptr)
  {
    return;
  }
  impl->cb.capabilities(impl->cb.user_data, caps_out);
}

// ============================================================
// Logger callback adapter
// ============================================================

namespace
{

class CapiCallbackLogger : public flox::ILogger
{
 public:
  CapiCallbackLogger(FloxLogCallback cb, void* ud) : _cb(cb), _ud(ud) {}

  void info(std::string_view msg) override { dispatch(0, msg); }
  void warn(std::string_view msg) override { dispatch(1, msg); }
  void error(std::string_view msg) override { dispatch(2, msg); }

 private:
  void dispatch(int32_t level, std::string_view msg)
  {
    // string_view is not guaranteed null-terminated; the C callback expects
    // const char*. Construct a std::string for the call duration.
    std::string s(msg);
    _cb(_ud, level, s.c_str());
  }

  FloxLogCallback _cb;
  void* _ud;
};

// Owned by the C API; outlives the registered call. Reset via
// flox_set_log_callback(NULL, ...).
std::unique_ptr<CapiCallbackLogger> g_capiLogger;

}  // namespace

void flox_set_log_callback(FloxLogCallback callback, void* user_data)
{
  if (callback == nullptr)
  {
    flox::setGlobalLogger(nullptr);
    g_capiLogger.reset();
    return;
  }
  auto next = std::make_unique<CapiCallbackLogger>(callback, user_data);
  flox::setGlobalLogger(next.get());
  // Replace AFTER swapping the pointer so concurrent log calls never see
  // a dangling adapter. If two flox_set_log_callback calls race, the loser
  // simply destroys its adapter without ever being installed.
  g_capiLogger = std::move(next);
}

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

void flox_runner_set_risk_manager(FloxRunnerHandle runner, FloxRiskManagerHandle rm)
{
  toRunner(runner)->setRiskManager(static_cast<capi_impl::FloxRiskManagerImpl*>(rm));
}

void flox_runner_set_kill_switch(FloxRunnerHandle runner, FloxKillSwitchHandle ks)
{
  toRunner(runner)->setKillSwitch(static_cast<capi_impl::FloxKillSwitchImpl*>(ks));
}

void flox_runner_set_order_validator(FloxRunnerHandle runner, FloxOrderValidatorHandle ov)
{
  toRunner(runner)->setOrderValidator(
      static_cast<capi_impl::FloxOrderValidatorImpl*>(ov));
}

void flox_runner_set_pnl_tracker(FloxRunnerHandle runner, FloxPnLTrackerHandle tracker)
{
  toRunner(runner)->setPnLTracker(
      static_cast<capi_impl::FloxPnLTrackerImpl*>(tracker));
}

void flox_runner_set_storage_sink(FloxRunnerHandle runner, FloxStorageSinkHandle sink)
{
  toRunner(runner)->setStorageSink(
      static_cast<capi_impl::FloxStorageSinkImpl*>(sink));
}

void flox_runner_set_executor(FloxRunnerHandle runner, FloxExecutorHandle executor)
{
  toRunner(runner)->setExecutor(
      static_cast<capi_impl::FloxExecutorImpl*>(executor));
}

void flox_runner_set_market_data_recorder(FloxRunnerHandle runner,
                                          FloxMarketDataRecorderHandle recorder)
{
  toRunner(runner)->setMarketDataRecorder(
      static_cast<capi_impl::FloxMarketDataRecorderImpl*>(recorder));
}

void flox_runner_attach_trace_recorder(FloxRunnerHandle runner, FloxRunRecorderHandle recorder)
{
  // recorder is a `flox::run::TraceRecorder*` (from
  // `flox_run_recorder_create`) or NULL to detach.
  toRunner(runner)->attachTraceRecorder(static_cast<void*>(recorder));
}

void flox_runner_set_trace_feed_ts_ns(FloxRunnerHandle runner, int64_t feed_ts_ns)
{
  toRunner(runner)->setTraceFeedTsNs(feed_ts_ns);
}

void flox_runner_trace_order_event(FloxRunnerHandle runner, uint64_t order_id,
                                   uint64_t parent_signal_id, uint32_t symbol_id,
                                   uint8_t event_kind, uint8_t side, uint8_t order_type,
                                   int64_t price_raw, int64_t qty_raw, uint32_t flags)
{
  auto* rec = toRunner(runner)->traceRecorder();
  if (!rec)
  {
    return;
  }
  flox::run::OrderEventView e;
  e.run_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  e.feed_ts_ns = toRunner(runner)->traceFeedTsNs();
  e.order_id = order_id;
  e.parent_signal_id = parent_signal_id;
  e.symbol_id = symbol_id;
  e.event_kind = static_cast<flox::run::OrderEventKind>(event_kind);
  e.side = side;
  e.order_type = order_type;
  e.price_raw = price_raw;
  e.qty_raw = qty_raw;
  e.flags = flags;
  rec->writeOrderEvent(e);
}

void flox_runner_trace_fill(FloxRunnerHandle runner, uint64_t order_id, uint64_t fill_id,
                            int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                            uint32_t symbol_id, uint8_t side, uint8_t liquidity)
{
  auto* rec = toRunner(runner)->traceRecorder();
  if (!rec)
  {
    return;
  }
  flox::run::FillView f;
  f.run_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  f.feed_ts_ns = toRunner(runner)->traceFeedTsNs();
  f.order_id = order_id;
  f.fill_id = fill_id;
  f.price_raw = price_raw;
  f.qty_raw = qty_raw;
  f.fee_raw = fee_raw;
  f.symbol_id = symbol_id;
  f.side = side;
  f.liquidity = static_cast<flox::run::FillLiquidity>(liquidity);
  rec->writeFill(f);
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

void flox_runner_on_bar(FloxRunnerHandle runner, uint32_t symbol,
                        uint8_t bar_type, uint64_t bar_type_param,
                        double open, double high, double low, double close,
                        double volume, double buy_volume,
                        int64_t start_time_ns, int64_t end_time_ns,
                        uint8_t close_reason)
{
  toRunner(runner)->onBar(symbol, bar_type, bar_type_param,
                          open, high, low, close, volume, buy_volume,
                          start_time_ns, end_time_ns, close_reason);
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

void flox_live_engine_set_risk_manager(FloxLiveEngineHandle engine, FloxRiskManagerHandle rm)
{
  toLiveEngine(engine)->setRiskManager(
      static_cast<capi_impl::FloxRiskManagerImpl*>(rm));
}

void flox_live_engine_set_kill_switch(FloxLiveEngineHandle engine, FloxKillSwitchHandle ks)
{
  toLiveEngine(engine)->setKillSwitch(
      static_cast<capi_impl::FloxKillSwitchImpl*>(ks));
}

void flox_live_engine_set_order_validator(FloxLiveEngineHandle engine,
                                          FloxOrderValidatorHandle ov)
{
  toLiveEngine(engine)->setOrderValidator(
      static_cast<capi_impl::FloxOrderValidatorImpl*>(ov));
}

void flox_live_engine_set_pnl_tracker(FloxLiveEngineHandle engine,
                                      FloxPnLTrackerHandle tracker)
{
  toLiveEngine(engine)->setPnLTracker(
      static_cast<capi_impl::FloxPnLTrackerImpl*>(tracker));
}

void flox_live_engine_set_storage_sink(FloxLiveEngineHandle engine,
                                       FloxStorageSinkHandle sink)
{
  toLiveEngine(engine)->setStorageSink(
      static_cast<capi_impl::FloxStorageSinkImpl*>(sink));
}

void flox_live_engine_set_market_data_recorder(FloxLiveEngineHandle engine,
                                               FloxMarketDataRecorderHandle recorder)
{
  toLiveEngine(engine)->setMarketDataRecorder(
      static_cast<capi_impl::FloxMarketDataRecorderImpl*>(recorder));
}

void flox_live_engine_set_executor(FloxLiveEngineHandle engine,
                                   FloxExecutorHandle executor)
{
  toLiveEngine(engine)->setExecutor(
      static_cast<capi_impl::FloxExecutorImpl*>(executor));
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

void flox_live_engine_publish_bar(FloxLiveEngineHandle engine,
                                  uint32_t symbol,
                                  uint8_t bar_type, uint64_t bar_type_param,
                                  double open, double high, double low, double close,
                                  double volume, double buy_volume,
                                  int64_t start_time_ns, int64_t end_time_ns,
                                  uint8_t close_reason)
{
  toLiveEngine(engine)->publishBar(symbol, bar_type, bar_type_param,
                                   open, high, low, close, volume, buy_volume,
                                   start_time_ns, end_time_ns, close_reason);
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

int flox_backtest_runner_run_tape(FloxBacktestRunnerHandle h,
                                  const char* tape_dir,
                                  FloxBacktestStats* out)
{
  return toBacktestRunner(h)->runTape(tape_dir, out);
}

int flox_backtest_runner_run_tapes(FloxBacktestRunnerHandle h,
                                   const char* const* tape_dirs,
                                   uint32_t n_dirs,
                                   FloxBacktestStats* out)
{
  return toBacktestRunner(h)->runTapes(tape_dirs, n_dirs, out);
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

int flox_backtest_runner_run_bars(FloxBacktestRunnerHandle h,
                                  const int64_t* start_time_ns,
                                  const int64_t* end_time_ns,
                                  const double* open,
                                  const double* high,
                                  const double* low,
                                  const double* close,
                                  const double* volume,
                                  uint32_t n,
                                  const char* symbol,
                                  uint8_t bar_type,
                                  uint64_t bar_type_param,
                                  FloxBacktestStats* out)
{
  return toBacktestRunner(h)->runFullBars(start_time_ns, end_time_ns,
                                          open, high, low, close, volume,
                                          n, symbol, bar_type, bar_type_param, out);
}

int flox_backtest_runner_run_replay_source(FloxBacktestRunnerHandle h,
                                           FloxReplaySourceHandle source,
                                           FloxBacktestStats* out)
{
  return toBacktestRunner(h)->runReplaySource(
      static_cast<capi_impl::FloxReplaySourceImpl*>(source), out);
}

FloxBacktestResultHandle flox_backtest_runner_take_result(FloxBacktestRunnerHandle h)
{
  auto* impl = toBacktestRunner(h);
  if (!impl->lastResult.has_value())
  {
    return nullptr;
  }
  auto* out = new FloxBacktestResultImpl();
  out->config = impl->lastResult->config();
  out->result = std::make_unique<BacktestResult>(*impl->lastResult);
  return static_cast<FloxBacktestResultHandle>(out);
}

void flox_backtest_runner_add_execution_listener(FloxBacktestRunnerHandle h,
                                                 FloxExecutionListenerHandle listener)
{
  toBacktestRunner(h)->addExecutionListener(
      static_cast<capi_impl::FloxExecutionListenerImpl*>(listener));
}

void flox_backtest_runner_add_journey_tracer(FloxBacktestRunnerHandle h,
                                             FloxOrderJourneyTracerHandle tracer)
{
  if (h == nullptr || tracer == nullptr)
  {
    return;
  }
  auto* runner = toBacktestRunner(h)->runner.get();
  runner->addExecutionListener(static_cast<flox::OrderJourneyTracer*>(tracer));
}

void flox_backtest_runner_set_executor(FloxBacktestRunnerHandle h,
                                       FloxExecutorHandle executor)
{
  toBacktestRunner(h)->setExecutor(
      static_cast<capi_impl::FloxExecutorImpl*>(executor));
}

// Pre-trade gate parity with the live runner. The Impl wrappers
// already inherit from the IRiskManager / IKillSwitch / IOrderValidator
// / IPnLTracker interfaces, so the cast handed to the BacktestRunner
// setters is direct.
void flox_backtest_runner_set_risk_manager(FloxBacktestRunnerHandle h,
                                           FloxRiskManagerHandle rm)
{
  toBacktestRunner(h)->setRiskManager(
      static_cast<capi_impl::FloxRiskManagerImpl*>(rm));
}

void flox_backtest_runner_set_kill_switch(FloxBacktestRunnerHandle h,
                                          FloxKillSwitchHandle ks)
{
  toBacktestRunner(h)->setKillSwitch(
      static_cast<capi_impl::FloxKillSwitchImpl*>(ks));
}

void flox_backtest_runner_set_order_validator(FloxBacktestRunnerHandle h,
                                              FloxOrderValidatorHandle ov)
{
  toBacktestRunner(h)->setOrderValidator(
      static_cast<capi_impl::FloxOrderValidatorImpl*>(ov));
}

void flox_backtest_runner_set_pnl_tracker(FloxBacktestRunnerHandle h,
                                          FloxPnLTrackerHandle tracker)
{
  toBacktestRunner(h)->setPnLTracker(
      static_cast<capi_impl::FloxPnLTrackerImpl*>(tracker));
}

// ============================================================
// Walk-forward (W6-T007 / T008)
// ============================================================

namespace
{

uint32_t resolveSymbolId(SymbolRegistry* reg, const char* symbol)
{
  if (!symbol || symbol[0] == '\0')
  {
    auto all = reg->getAllSymbols();
    return all.empty() ? 0 : all.front().id;
  }
  for (const auto& info : reg->getAllSymbols())
  {
    if (info.symbol == symbol)
    {
      return info.id;
    }
  }
  return 0;
}

int64_t normalizeWfTs(int64_t t)
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

std::vector<OhlcvReplaySource::Bar> loadOhlcvBarsCsv(const char* path,
                                                     uint32_t symbolId)
{
  std::vector<OhlcvReplaySource::Bar> out;
  std::ifstream f(path);
  if (!f.is_open())
  {
    return out;
  }
  std::string line;
  std::getline(f, line);  // header
  while (std::getline(f, line))
  {
    if (line.empty())
    {
      continue;
    }
    std::istringstream ss(line);
    std::string tok;
    std::getline(ss, tok, ',');
    int64_t ts = normalizeWfTs(std::stoll(tok));
    std::getline(ss, tok, ',');  // open
    std::getline(ss, tok, ',');  // high
    std::getline(ss, tok, ',');  // low
    std::getline(ss, tok, ',');
    double c = std::stod(tok);
    OhlcvReplaySource::Bar bar;
    bar.ts_ns = ts;
    bar.price_raw = Price::fromDouble(c).raw();
    bar.symbol_id = symbolId;
    out.push_back(bar);
  }
  return out;
}

void fillStatsStruct(const BacktestStats& s, FloxBacktestStats* out)
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
  out->avgTradeDurationNs = s.avgTradeDurationNs;
  out->medianTradeDurationNs = s.medianTradeDurationNs;
  out->maxTradeDurationNs = s.maxTradeDurationNs;
  out->sharpeRatio = s.sharpeRatio;
  out->sortinoRatio = s.sortinoRatio;
  out->calmarRatio = s.calmarRatio;
  out->timeWeightedReturn = s.timeWeightedReturn;
  out->returnPct = s.returnPct;
  out->startTimeNs = static_cast<int64_t>(s.startTimeNs);
  out->endTimeNs = static_cast<int64_t>(s.endTimeNs);
}

}  // namespace

uint32_t flox_walk_forward_run_csv(FloxRegistryHandle reg_handle,
                                   const char* csv_path, const char* symbol,
                                   double fee_rate, double initial_capital,
                                   const FloxWalkForwardConfig* cfg,
                                   FloxWalkForwardFactoryFn factory,
                                   void* user_data,
                                   FloxWalkForwardFold* folds_out,
                                   uint32_t max_folds)
{
  if (!cfg || !csv_path || !factory)
  {
    return 0;
  }
  SymbolRegistry* reg = toRegistry(reg_handle);
  uint32_t symId = resolveSymbolId(reg, symbol);
  std::vector<OhlcvReplaySource::Bar> bars = loadOhlcvBarsCsv(csv_path, symId);
  if (bars.empty())
  {
    return 0;
  }

  BacktestConfig bcfg{};
  bcfg.feeRate = fee_rate;
  bcfg.initialCapital = initial_capital;
  bcfg.usePercentageFee = true;

  WalkForwardConfig wcfg{};
  wcfg.mode = (cfg->mode == 0) ? WalkForwardMode::Anchored
                               : WalkForwardMode::Sliding;
  wcfg.trainSize = cfg->train_size;
  wcfg.testSize = cfg->test_size;
  wcfg.step = cfg->step;
  wcfg.minTrainSize = cfg->min_train_size;

  // If the caller only wants a count, compute it from the config + bar
  // count without invoking the factory or running anything. This lets
  // bindings size the output buffer cheaply.
  if (!folds_out || max_folds == 0)
  {
    const uint64_t n = static_cast<uint64_t>(bars.size());
    const uint64_t step = wcfg.step == 0 ? wcfg.testSize : wcfg.step;
    if (wcfg.testSize == 0 || step == 0)
    {
      return 0;
    }
    if (wcfg.mode == WalkForwardMode::Anchored)
    {
      const uint64_t firstSplit =
          wcfg.minTrainSize > 0 ? wcfg.minTrainSize : wcfg.testSize;
      if (firstSplit + wcfg.testSize > n)
      {
        return 0;
      }
      return static_cast<uint32_t>(
          (n - firstSplit - wcfg.testSize) / step + 1);
    }
    if (wcfg.trainSize + wcfg.testSize > n)
    {
      return 0;
    }
    return static_cast<uint32_t>(
        (n - wcfg.trainSize - wcfg.testSize) / step + 1);
  }

  WalkForwardRunner wfr(bcfg, wcfg);
  wfr.setStrategyFactory([factory, user_data](std::size_t foldIdx) -> IStrategy*
                         {
                           FloxStrategyHandle h = factory(user_data, foldIdx);
                           return reinterpret_cast<BridgeStrategy*>(h); });

  auto folds = wfr.run(bars);
  const uint32_t total = static_cast<uint32_t>(folds.size());
  const uint32_t n = (total < max_folds) ? total : max_folds;
  for (uint32_t i = 0; i < n; ++i)
  {
    const auto& f = folds[i];
    folds_out[i].fold_index = f.foldIndex;
    folds_out[i].train_start_bar = f.trainStartBar;
    folds_out[i].train_end_bar = f.trainEndBar;
    folds_out[i].test_start_bar = f.testStartBar;
    folds_out[i].test_end_bar = f.testEndBar;
    folds_out[i].train_start_ns = static_cast<int64_t>(f.trainStartNs);
    folds_out[i].train_end_ns = static_cast<int64_t>(f.trainEndNs);
    folds_out[i].test_start_ns = static_cast<int64_t>(f.testStartNs);
    folds_out[i].test_end_ns = static_cast<int64_t>(f.testEndNs);
    fillStatsStruct(f.trainStats, &folds_out[i].train_stats);
    fillStatsStruct(f.testStats, &folds_out[i].test_stats);
  }
  return n;
}

// ============================================================
// Grid search (W6-T002 sequential)
// ============================================================

struct FloxGridSearchImpl
{
  GridSearch core;
};

FloxGridSearchHandle flox_grid_search_create()
{
  return static_cast<FloxGridSearchHandle>(new FloxGridSearchImpl());
}

void flox_grid_search_destroy(FloxGridSearchHandle gs)
{
  delete static_cast<FloxGridSearchImpl*>(gs);
}

void flox_grid_search_add_axis(FloxGridSearchHandle gs,
                               const double* values, uint32_t num_values)
{
  if (!gs || !values)
  {
    return;
  }
  std::vector<double> v(values, values + num_values);
  static_cast<FloxGridSearchImpl*>(gs)->core.addAxis(std::move(v));
}

uint64_t flox_grid_search_total(FloxGridSearchHandle gs)
{
  return gs ? static_cast<FloxGridSearchImpl*>(gs)->core.totalCombinations() : 0;
}

uint32_t flox_grid_search_params_for_index(FloxGridSearchHandle gs,
                                           uint64_t index,
                                           double* params_out,
                                           uint32_t max_params)
{
  if (!gs)
  {
    return 0;
  }
  auto p = static_cast<FloxGridSearchImpl*>(gs)->core.paramsForIndex(index);
  if (!params_out)
  {
    return static_cast<uint32_t>(p.size());
  }
  const uint32_t n = std::min(static_cast<uint32_t>(p.size()), max_params);
  for (uint32_t i = 0; i < n; ++i)
  {
    params_out[i] = p[i];
  }
  return n;
}

uint64_t flox_grid_search_run(FloxGridSearchHandle gs,
                              FloxGridSearchFactoryFn factory,
                              void* user_data,
                              FloxBacktestStats* stats_out,
                              uint32_t max_results)
{
  if (!gs || !factory)
  {
    return 0;
  }
  auto* impl = static_cast<FloxGridSearchImpl*>(gs);
  const uint64_t total = impl->core.totalCombinations();
  if (!stats_out)
  {
    return total;
  }
  // Run sequentially via direct calls to factory; we don't go through
  // GridSearch::run because the factory expects a C-side stats fill,
  // not a BacktestResult.
  const uint64_t n = std::min<uint64_t>(total, max_results);
  for (uint64_t i = 0; i < n; ++i)
  {
    auto params = impl->core.paramsForIndex(i);
    FloxBacktestStats s{};
    factory(user_data, i, params.data(),
            static_cast<uint32_t>(params.size()), &s);
    stats_out[i] = s;
  }
  return n;
}

// ============================================================
// Heatmap rendering (W6-T004)
// ============================================================

uint64_t flox_render_heatmap_html(const FloxHeatmapData* data,
                                  char* out_buf, uint64_t max_size)
{
  if (!data || data->z == nullptr || data->rows == 0 || data->cols == 0)
  {
    return 0;
  }
  flox::report::HeatmapData hd;
  const std::size_t n = static_cast<std::size_t>(data->rows) * data->cols;
  hd.z.assign(data->z, data->z + n);
  hd.rows = data->rows;
  hd.cols = data->cols;
  if (data->row_labels && data->num_row_labels > 0)
  {
    hd.rowLabels.reserve(data->num_row_labels);
    for (uint32_t i = 0; i < data->num_row_labels; ++i)
    {
      hd.rowLabels.emplace_back(data->row_labels[i] ? data->row_labels[i] : "");
    }
  }
  if (data->col_labels && data->num_col_labels > 0)
  {
    hd.colLabels.reserve(data->num_col_labels);
    for (uint32_t i = 0; i < data->num_col_labels; ++i)
    {
      hd.colLabels.emplace_back(data->col_labels[i] ? data->col_labels[i] : "");
    }
  }
  if (data->title)
  {
    hd.title = data->title;
  }
  if (data->x_axis_name)
  {
    hd.xAxisName = data->x_axis_name;
  }
  if (data->y_axis_name)
  {
    hd.yAxisName = data->y_axis_name;
  }
  if (data->metric_name)
  {
    hd.metricName = data->metric_name;
  }

  std::string html = flox::report::renderHeatmapHtml(hd);
  const uint64_t total = static_cast<uint64_t>(html.size());
  if (!out_buf)
  {
    return total;
  }
  const uint64_t to_copy = (total < max_size) ? total : max_size;
  std::memcpy(out_buf, html.data(), to_copy);
  return total;
}

// ── Latency models ────────────────────────────────────────────────

namespace
{
flox::LatencyModel* asLatency(FloxLatencyModelHandle h)
{
  return static_cast<flox::LatencyModel*>(h);
}
}  // namespace

extern "C" FloxLatencyModelHandle flox_latency_constant_create(int64_t feed_ns,
                                                               int64_t order_ns,
                                                               int64_t fill_ns)
{
  try
  {
    return new flox::ConstantLatency(feed_ns, order_ns, fill_ns);
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" FloxLatencyModelHandle flox_latency_gaussian_create(double feed_mean_ns,
                                                               double feed_stddev_ns,
                                                               double order_mean_ns,
                                                               double order_stddev_ns,
                                                               double fill_mean_ns,
                                                               double fill_stddev_ns,
                                                               uint64_t seed)
{
  try
  {
    return new flox::GaussianLatency(feed_mean_ns, feed_stddev_ns,
                                     order_mean_ns, order_stddev_ns,
                                     fill_mean_ns, fill_stddev_ns, seed);
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" FloxLatencyModelHandle flox_latency_exponential_create(double feed_mean_ns,
                                                                  double order_mean_ns,
                                                                  double fill_mean_ns,
                                                                  uint64_t seed)
{
  try
  {
    return new flox::ExponentialLatency(feed_mean_ns, order_mean_ns, fill_mean_ns, seed);
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" FloxLatencyModelHandle flox_latency_empirical_create(const int64_t* feed_samples,
                                                                size_t feed_count,
                                                                const int64_t* order_samples,
                                                                size_t order_count,
                                                                const int64_t* fill_samples,
                                                                size_t fill_count,
                                                                uint64_t seed)
{
  try
  {
    std::vector<int64_t> feed(feed_samples, feed_samples + feed_count);
    std::vector<int64_t> order(order_samples, order_samples + order_count);
    std::vector<int64_t> fill(fill_samples, fill_samples + fill_count);
    return new flox::EmpiricalLatency(std::move(feed), std::move(order),
                                      std::move(fill), seed);
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" void flox_latency_destroy(FloxLatencyModelHandle model)
{
  delete asLatency(model);
}

extern "C" int64_t flox_latency_feed_delay(FloxLatencyModelHandle model)
{
  return model ? asLatency(model)->feedDelay() : 0;
}

extern "C" int64_t flox_latency_order_delay(FloxLatencyModelHandle model)
{
  return model ? asLatency(model)->orderDelay() : 0;
}

extern "C" int64_t flox_latency_fill_delay(FloxLatencyModelHandle model)
{
  return model ? asLatency(model)->fillDelay() : 0;
}

extern "C" void flox_latency_sample(FloxLatencyModelHandle model, FloxLatencySample* out)
{
  if (!model || !out)
  {
    return;
  }
  flox::LatencySample s = asLatency(model)->sample();
  out->feed_ns = s.feed_ns;
  out->order_ns = s.order_ns;
  out->fill_ns = s.fill_ns;
}

extern "C" void flox_latency_reset(FloxLatencyModelHandle model, uint64_t seed)
{
  if (model)
  {
    asLatency(model)->reset(seed);
  }
}

// ── Tape diff ─────────────────────────────────────────────────────

namespace
{
flox::replay::TapeDiffResult* asDiff(FloxTapeDiffHandle h)
{
  return static_cast<flox::replay::TapeDiffResult*>(h);
}
}  // namespace

extern "C" FloxTapeDiffHandle flox_tape_diff_create(const char* left_path,
                                                    const char* right_path,
                                                    uint32_t max_mismatches,
                                                    int64_t field_tolerance_ns)
{
  if (!left_path || !right_path)
  {
    return nullptr;
  }
  if (!std::filesystem::is_directory(left_path) ||
      !std::filesystem::is_directory(right_path))
  {
    return nullptr;
  }
  try
  {
    flox::replay::TapeDiffOptions opts;
    opts.max_mismatches = max_mismatches;
    opts.field_tolerance_ns = field_tolerance_ns;
    auto* result = new flox::replay::TapeDiffResult(
        flox::replay::diffTapes(left_path, right_path, opts));
    return result;
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" void flox_tape_diff_destroy(FloxTapeDiffHandle handle)
{
  delete asDiff(handle);
}

extern "C" uint64_t flox_tape_diff_left_count(FloxTapeDiffHandle handle)
{
  return handle ? asDiff(handle)->left_count : 0;
}

extern "C" uint64_t flox_tape_diff_right_count(FloxTapeDiffHandle handle)
{
  return handle ? asDiff(handle)->right_count : 0;
}

extern "C" uint8_t flox_tape_diff_first_divergence(FloxTapeDiffHandle handle,
                                                   uint64_t* out_index)
{
  if (!handle)
  {
    return 0;
  }
  auto& r = *asDiff(handle);
  if (!r.first_divergence_index.has_value())
  {
    return 0;
  }
  if (out_index)
  {
    *out_index = *r.first_divergence_index;
  }
  return 1;
}

extern "C" uint8_t flox_tape_diff_equal(FloxTapeDiffHandle handle)
{
  return (handle && asDiff(handle)->equal) ? 1 : 0;
}

extern "C" uint64_t flox_tape_diff_mismatch_count(FloxTapeDiffHandle handle)
{
  return handle ? asDiff(handle)->mismatches.size() : 0;
}

extern "C" uint64_t flox_tape_diff_copy_mismatches(FloxTapeDiffHandle handle,
                                                   FloxTapeDiffMismatch* out,
                                                   uint64_t max_entries)
{
  if (!handle)
  {
    return 0;
  }
  auto& r = *asDiff(handle);
  const uint64_t total = r.mismatches.size();
  if (!out)
  {
    return total;
  }
  const uint64_t to_copy = std::min<uint64_t>(total, max_entries);
  for (uint64_t i = 0; i < to_copy; ++i)
  {
    const auto& m = r.mismatches[i];
    out[i].index = m.index;
    out[i].left.exchange_ts_ns = m.left.exchange_ts_ns;
    out[i].left.price_raw = m.left.price_raw;
    out[i].left.qty_raw = m.left.qty_raw;
    out[i].left.symbol_id = m.left.symbol_id;
    out[i].left.side = m.left.side;
    out[i].right.exchange_ts_ns = m.right.exchange_ts_ns;
    out[i].right.price_raw = m.right.price_raw;
    out[i].right.qty_raw = m.right.qty_raw;
    out[i].right.symbol_id = m.right.symbol_id;
    out[i].right.side = m.right.side;
  }
  return to_copy;
}

// ── Portfolio risk aggregator ─────────────────────────────────────

namespace
{
flox::risk::PortfolioRiskAggregator* asPortfolio(FloxPortfolioRiskHandle h)
{
  return static_cast<flox::risk::PortfolioRiskAggregator*>(h);
}

flox::risk::RiskRules unpackRules(const FloxPortfolioRiskRules* r)
{
  flox::risk::RiskRules out;
  if (!r)
  {
    return out;
  }
  if (r->has_max_drawdown_pct)
  {
    out.max_drawdown_pct = r->max_drawdown_pct;
  }
  if (r->has_max_daily_loss)
  {
    out.max_daily_loss = r->max_daily_loss;
  }
  if (r->has_max_gross_exposure)
  {
    out.max_gross_exposure = r->max_gross_exposure;
  }
  if (r->has_max_concentration_pct)
  {
    out.max_concentration_pct = r->max_concentration_pct;
  }
  return out;
}
}  // namespace

extern "C" FloxPortfolioRiskHandle flox_portfolio_risk_create(
    const FloxPortfolioRiskRules* rules, double initial_equity)
{
  try
  {
    return new flox::risk::PortfolioRiskAggregator(unpackRules(rules), initial_equity);
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" void flox_portfolio_risk_destroy(FloxPortfolioRiskHandle handle)
{
  delete asPortfolio(handle);
}

extern "C" void flox_portfolio_risk_update(FloxPortfolioRiskHandle handle,
                                           const char* name,
                                           const FloxStrategyAccountFields* fields,
                                           uint8_t field_mask)
{
  if (!handle || !name || !fields)
  {
    return;
  }
  flox::risk::StrategyAccount row;
  row.name = name;
  row.realized_pnl = fields->realized_pnl;
  row.unrealized_pnl = fields->unrealized_pnl;
  row.fees = fields->fees;
  row.gross_exposure = fields->gross_exposure;
  row.net_exposure = fields->net_exposure;
  row.trade_count = fields->trade_count;
  asPortfolio(handle)->update(name, row, field_mask);
}

extern "C" void flox_portfolio_risk_remove(FloxPortfolioRiskHandle handle, const char* name)
{
  if (!handle || !name)
  {
    return;
  }
  asPortfolio(handle)->remove(name);
}

extern "C" void flox_portfolio_risk_reset_kill_switch(FloxPortfolioRiskHandle handle)
{
  if (handle)
  {
    asPortfolio(handle)->resetKillSwitch();
  }
}

namespace
{
// Per-handle scratch storage for FloxBreach string lifetime. Held by
// the C ABI shim; re-populated on every call that surfaces a Breach
// to the caller. Matches the documented invalidation contract.
struct PortfolioBreachScratch
{
  std::vector<flox::risk::Breach> breaches;
};

thread_local std::map<FloxPortfolioRiskHandle, PortfolioBreachScratch> g_breach_scratch;

void writeBreach(FloxBreach* out, const flox::risk::Breach& b)
{
  if (!out)
  {
    return;
  }
  out->rule = b.rule.c_str();
  out->value = b.value;
  out->limit = b.limit;
  out->detail = b.detail.c_str();
}
}  // namespace

extern "C" uint8_t flox_portfolio_risk_check_order(FloxPortfolioRiskHandle handle,
                                                   const char* strategy, double notional,
                                                   const char* side, FloxBreach* out_breach)
{
  if (!handle)
  {
    return 0;
  }
  auto opt = asPortfolio(handle)->checkOrder(strategy ? strategy : "",
                                             notional, side ? side : "");
  if (!opt.has_value())
  {
    return 0;
  }
  auto& scratch = g_breach_scratch[handle];
  scratch.breaches = {*opt};
  writeBreach(out_breach, scratch.breaches.front());
  return 1;
}

extern "C" double flox_portfolio_risk_total_daily_pnl(FloxPortfolioRiskHandle handle)
{
  return handle ? asPortfolio(handle)->snapshot().total_daily_pnl : 0.0;
}

extern "C" double flox_portfolio_risk_total_gross_exposure(FloxPortfolioRiskHandle handle)
{
  return handle ? asPortfolio(handle)->snapshot().total_gross_exposure : 0.0;
}

extern "C" double flox_portfolio_risk_current_equity(FloxPortfolioRiskHandle handle)
{
  return handle ? asPortfolio(handle)->snapshot().current_equity : 0.0;
}

extern "C" double flox_portfolio_risk_drawdown_pct(FloxPortfolioRiskHandle handle)
{
  return handle ? asPortfolio(handle)->snapshot().drawdown_pct : 0.0;
}

extern "C" uint8_t flox_portfolio_risk_kill_switch_active(FloxPortfolioRiskHandle handle)
{
  return (handle && asPortfolio(handle)->snapshot().kill_switch_active) ? 1 : 0;
}

extern "C" uint64_t flox_portfolio_risk_breach_count(FloxPortfolioRiskHandle handle)
{
  if (!handle)
  {
    return 0;
  }
  auto snap = asPortfolio(handle)->snapshot();
  auto& scratch = g_breach_scratch[handle];
  scratch.breaches = std::move(snap.active_breaches);
  return scratch.breaches.size();
}

extern "C" uint8_t flox_portfolio_risk_breach_at(FloxPortfolioRiskHandle handle,
                                                 uint64_t index, FloxBreach* out)
{
  if (!handle || !out)
  {
    return 0;
  }
  auto it = g_breach_scratch.find(handle);
  if (it == g_breach_scratch.end())
  {
    return 0;
  }
  if (index >= it->second.breaches.size())
  {
    return 0;
  }
  writeBreach(out, it->second.breaches[index]);
  return 1;
}

extern "C" uint64_t flox_portfolio_risk_account_count(FloxPortfolioRiskHandle handle)
{
  return handle ? asPortfolio(handle)->snapshot().accounts.size() : 0;
}

// ── Execution algorithms ──────────────────────────────────────────

namespace
{
flox::execution::ExecutionAlgo* asAlgo(FloxExecAlgoHandle h)
{
  return static_cast<flox::execution::ExecutionAlgo*>(h);
}
}  // namespace

extern "C" FloxExecAlgoHandle flox_exec_twap_create(double target_qty, uint8_t side,
                                                    uint32_t symbol, uint8_t type,
                                                    double limit_price,
                                                    int64_t duration_ns,
                                                    uint32_t slice_count,
                                                    int64_t start_time_ns)
{
  try
  {
    return new flox::execution::TWAPExecutor(
        target_qty,
        static_cast<flox::execution::Side>(side),
        symbol,
        static_cast<flox::execution::OrderType>(type),
        limit_price,
        duration_ns, slice_count, start_time_ns);
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" FloxExecAlgoHandle flox_exec_vwap_create(double target_qty, uint8_t side,
                                                    uint32_t symbol, uint8_t type,
                                                    double limit_price,
                                                    const int64_t* volume_curve_ts,
                                                    const double* volume_curve_vol,
                                                    size_t n)
{
  try
  {
    std::vector<std::pair<int64_t, double>> curve;
    curve.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
      curve.emplace_back(volume_curve_ts[i], volume_curve_vol[i]);
    }
    return new flox::execution::VWAPExecutor(
        target_qty,
        static_cast<flox::execution::Side>(side),
        symbol,
        static_cast<flox::execution::OrderType>(type),
        limit_price,
        std::move(curve));
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" FloxExecAlgoHandle flox_exec_iceberg_create(double target_qty, uint8_t side,
                                                       uint32_t symbol, uint8_t type,
                                                       double limit_price,
                                                       double visible_qty)
{
  try
  {
    return new flox::execution::IcebergExecutor(
        target_qty,
        static_cast<flox::execution::Side>(side),
        symbol,
        static_cast<flox::execution::OrderType>(type),
        limit_price, visible_qty);
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" FloxExecAlgoHandle flox_exec_pov_create(double target_qty, uint8_t side,
                                                   uint32_t symbol, uint8_t type,
                                                   double limit_price,
                                                   double participation_rate,
                                                   double min_slice_qty)
{
  try
  {
    return new flox::execution::POVExecutor(
        target_qty,
        static_cast<flox::execution::Side>(side),
        symbol,
        static_cast<flox::execution::OrderType>(type),
        limit_price, participation_rate, min_slice_qty);
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" void flox_exec_destroy(FloxExecAlgoHandle handle) { delete asAlgo(handle); }

extern "C" void flox_exec_step(FloxExecAlgoHandle handle, int64_t now_ns)
{
  if (handle)
  {
    asAlgo(handle)->step(now_ns);
  }
}

extern "C" void flox_exec_report_fill(FloxExecAlgoHandle handle, double qty)
{
  if (handle)
  {
    asAlgo(handle)->reportFill(qty);
  }
}

extern "C" void flox_exec_observe_volume(FloxExecAlgoHandle handle, double qty)
{
  if (handle)
  {
    asAlgo(handle)->observeVolume(qty);
  }
}

extern "C" size_t flox_exec_pending_count(FloxExecAlgoHandle handle)
{
  return handle ? asAlgo(handle)->pending().size() : 0;
}

extern "C" uint8_t flox_exec_pending_at(FloxExecAlgoHandle handle, size_t index,
                                        FloxExecChildOrder* out)
{
  if (!handle || !out)
  {
    return 0;
  }
  const auto& p = asAlgo(handle)->pending();
  if (index >= p.size())
  {
    return 0;
  }
  const auto& c = p[index];
  out->order_id = c.order_id;
  out->timestamp_ns = c.timestamp_ns;
  out->qty = c.qty;
  out->price = c.price;
  out->type = static_cast<uint8_t>(c.type);
  return 1;
}

extern "C" void flox_exec_clear_pending(FloxExecAlgoHandle handle)
{
  if (handle)
  {
    asAlgo(handle)->clearPending();
  }
}

extern "C" double flox_exec_target_qty(FloxExecAlgoHandle handle)
{
  return handle ? asAlgo(handle)->targetQty() : 0.0;
}

extern "C" double flox_exec_submitted_qty(FloxExecAlgoHandle handle)
{
  return handle ? asAlgo(handle)->submittedQty() : 0.0;
}

extern "C" double flox_exec_filled_qty(FloxExecAlgoHandle handle)
{
  return handle ? asAlgo(handle)->filledQty() : 0.0;
}

extern "C" double flox_exec_remaining_qty(FloxExecAlgoHandle handle)
{
  return handle ? asAlgo(handle)->remainingQty() : 0.0;
}

extern "C" uint8_t flox_exec_is_done(FloxExecAlgoHandle handle)
{
  return (handle && asAlgo(handle)->isDone()) ? 1 : 0;
}

// ── Delta book compression ────────────────────────────────────────

namespace
{
flox::replay::DeltaBookEncoder* asEncoder(FloxDeltaBookEncoderHandle h)
{
  return static_cast<flox::replay::DeltaBookEncoder*>(h);
}
flox::replay::DeltaBookReplayer* asReplayer(FloxDeltaBookReplayerHandle h)
{
  return static_cast<flox::replay::DeltaBookReplayer*>(h);
}

std::vector<flox::replay::BookLevel> toLevels(const FloxBookLevel* arr, size_t n)
{
  std::vector<flox::replay::BookLevel> out(n);
  for (size_t i = 0; i < n; ++i)
  {
    out[i].price_raw = arr[i].price_raw;
    out[i].qty_raw = arr[i].quantity_raw;
  }
  return out;
}

uint64_t copyLevelsTo(const std::vector<flox::replay::BookLevel>& src,
                      FloxBookLevel* out, uint64_t max_entries)
{
  const uint64_t n = std::min<uint64_t>(src.size(), max_entries);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i].price_raw = src[i].price_raw;
    out[i].quantity_raw = src[i].qty_raw;
  }
  return n;
}

// Per-handle scratch for last encode/replay output. The C ABI returns
// counts plus a follow-up copy() call, matching the pattern used by
// flox_tape_diff and flox_portfolio_risk for variable-length data.
struct EncoderScratch
{
  std::vector<flox::replay::BookLevel> bids;
  std::vector<flox::replay::BookLevel> asks;
};

thread_local std::map<FloxDeltaBookEncoderHandle, EncoderScratch> g_encoder_scratch;
thread_local std::map<FloxDeltaBookReplayerHandle, EncoderScratch> g_replayer_scratch;
}  // namespace

extern "C" FloxDeltaBookEncoderHandle flox_delta_book_encoder_create(uint32_t anchor_every)
{
  try
  {
    return new flox::replay::DeltaBookEncoder(anchor_every);
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" void flox_delta_book_encoder_destroy(FloxDeltaBookEncoderHandle handle)
{
  g_encoder_scratch.erase(handle);
  delete asEncoder(handle);
}

extern "C" void flox_delta_book_encoder_reset(FloxDeltaBookEncoderHandle handle, uint32_t symbol_id)
{
  if (handle)
  {
    asEncoder(handle)->reset(symbol_id);
  }
}

extern "C" void flox_delta_book_encoder_reset_all(FloxDeltaBookEncoderHandle handle)
{
  if (handle)
  {
    asEncoder(handle)->resetAll();
  }
}

extern "C" void flox_delta_book_encoder_encode(FloxDeltaBookEncoderHandle handle,
                                               uint32_t symbol_id,
                                               const FloxBookLevel* bids, size_t bid_count,
                                               const FloxBookLevel* asks, size_t ask_count,
                                               uint8_t* out_is_delta,
                                               uint64_t* out_bid_count,
                                               uint64_t* out_ask_count)
{
  if (!handle)
  {
    return;
  }
  auto bids_in = toLevels(bids, bid_count);
  auto asks_in = toLevels(asks, ask_count);
  auto result = asEncoder(handle)->encode(symbol_id, bids_in, asks_in);
  auto& scratch = g_encoder_scratch[handle];
  scratch.bids = std::move(result.bids);
  scratch.asks = std::move(result.asks);
  if (out_is_delta)
  {
    *out_is_delta = result.is_delta ? 1 : 0;
  }
  if (out_bid_count)
  {
    *out_bid_count = scratch.bids.size();
  }
  if (out_ask_count)
  {
    *out_ask_count = scratch.asks.size();
  }
}

extern "C" uint64_t flox_delta_book_encoder_copy_bids(FloxDeltaBookEncoderHandle handle,
                                                      FloxBookLevel* out, uint64_t max_entries)
{
  if (!handle || !out)
  {
    return 0;
  }
  auto it = g_encoder_scratch.find(handle);
  if (it == g_encoder_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.bids, out, max_entries);
}

extern "C" uint64_t flox_delta_book_encoder_copy_asks(FloxDeltaBookEncoderHandle handle,
                                                      FloxBookLevel* out, uint64_t max_entries)
{
  if (!handle || !out)
  {
    return 0;
  }
  auto it = g_encoder_scratch.find(handle);
  if (it == g_encoder_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.asks, out, max_entries);
}

extern "C" FloxDeltaBookReplayerHandle flox_delta_book_replayer_create(void)
{
  try
  {
    return new flox::replay::DeltaBookReplayer();
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" void flox_delta_book_replayer_destroy(FloxDeltaBookReplayerHandle handle)
{
  g_replayer_scratch.erase(handle);
  delete asReplayer(handle);
}

extern "C" void flox_delta_book_replayer_reset(FloxDeltaBookReplayerHandle handle, uint32_t symbol_id)
{
  if (handle)
  {
    asReplayer(handle)->reset(symbol_id);
  }
}

extern "C" void flox_delta_book_replayer_apply(FloxDeltaBookReplayerHandle handle,
                                               uint8_t type, uint32_t symbol_id,
                                               const FloxBookLevel* bids, size_t bid_count,
                                               const FloxBookLevel* asks, size_t ask_count,
                                               uint64_t* out_bid_count,
                                               uint64_t* out_ask_count)
{
  if (!handle)
  {
    return;
  }
  auto bids_in = toLevels(bids, bid_count);
  auto asks_in = toLevels(asks, ask_count);
  auto snap = asReplayer(handle)->apply(type, symbol_id, bids_in, asks_in);
  auto& scratch = g_replayer_scratch[handle];
  scratch.bids = std::move(snap.bids);
  scratch.asks = std::move(snap.asks);
  if (out_bid_count)
  {
    *out_bid_count = scratch.bids.size();
  }
  if (out_ask_count)
  {
    *out_ask_count = scratch.asks.size();
  }
}

extern "C" uint64_t flox_delta_book_replayer_copy_bids(FloxDeltaBookReplayerHandle handle,
                                                       FloxBookLevel* out, uint64_t max_entries)
{
  if (!handle || !out)
  {
    return 0;
  }
  auto it = g_replayer_scratch.find(handle);
  if (it == g_replayer_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.bids, out, max_entries);
}

extern "C" uint64_t flox_delta_book_replayer_copy_asks(FloxDeltaBookReplayerHandle handle,
                                                       FloxBookLevel* out, uint64_t max_entries)
{
  if (!handle || !out)
  {
    return 0;
  }
  auto it = g_replayer_scratch.find(handle);
  if (it == g_replayer_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.asks, out, max_entries);
}

// ============================================================
// Strategy run trace (.floxrun) — implementation
// ============================================================

namespace
{

inline flox::run::TraceRecorder* asRecorder(FloxRunRecorderHandle h)
{
  return static_cast<flox::run::TraceRecorder*>(h);
}

struct RunReaderState
{
  std::unique_ptr<flox::run::TraceReader> reader;
  std::vector<flox::run::OwnedSignal> signals;
  std::vector<flox::run::OwnedOrderEvent> orders;
  std::vector<flox::run::FillRecord> fills;
};

inline RunReaderState* asReader(FloxRunReaderHandle h)
{
  return static_cast<RunReaderState*>(h);
}

uint64_t copyStringTo(const std::string& src, char* out, uint64_t max_bytes)
{
  if (out == nullptr || max_bytes == 0)
  {
    return src.size();
  }
  uint64_t n = std::min(static_cast<uint64_t>(src.size()), max_bytes);
  std::memcpy(out, src.data(), n);
  return n;
}

}  // namespace

extern "C" FloxRunRecorderHandle flox_run_recorder_create(const char* path,
                                                          const char* strategy_id,
                                                          const char* strategy_hash,
                                                          int64_t run_started_ns)
{
  if (path == nullptr)
  {
    return nullptr;
  }
  flox::run::TraceRecorderOptions opts;
  opts.strategy_id = strategy_id ? strategy_id : "";
  opts.strategy_hash = strategy_hash ? strategy_hash : "";
  opts.run_started_ns = run_started_ns;
  try
  {
    return new flox::run::TraceRecorder(path, std::move(opts));
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" void flox_run_recorder_destroy(FloxRunRecorderHandle handle)
{
  delete asRecorder(handle);
}

extern "C" void flox_run_recorder_add_tape_ref(FloxRunRecorderHandle handle,
                                               const char* path,
                                               const char* content_hash,
                                               int64_t first_event_ns,
                                               int64_t last_event_ns)
{
  auto* rec = asRecorder(handle);
  if (rec == nullptr || path == nullptr)
  {
    return;
  }
  flox::run::TapeRef ref;
  ref.path = path;
  ref.content_hash = content_hash ? content_hash : "";
  ref.first_event_ns = first_event_ns;
  ref.last_event_ns = last_event_ns;
  rec->addTapeRef(std::move(ref));
}

extern "C" void flox_run_recorder_set_run_ended_ns(FloxRunRecorderHandle handle, int64_t ns)
{
  if (auto* rec = asRecorder(handle))
  {
    rec->setRunEndedNs(ns);
  }
}

extern "C" void flox_run_recorder_write_signal(FloxRunRecorderHandle handle,
                                               int64_t run_ts_ns, int64_t feed_ts_ns,
                                               uint32_t signal_id, uint32_t flags,
                                               int64_t strength_raw,
                                               const char* name, size_t name_len,
                                               const uint32_t* symbol_ids, size_t symbol_count,
                                               const uint8_t* payload, size_t payload_len)
{
  auto* rec = asRecorder(handle);
  if (rec == nullptr)
  {
    return;
  }
  flox::run::SignalView s;
  s.run_ts_ns = run_ts_ns;
  s.feed_ts_ns = feed_ts_ns;
  s.signal_id = signal_id;
  s.flags = flags;
  s.strength_raw = strength_raw;
  if (name && name_len > 0)
  {
    s.name = std::string_view(name, name_len);
  }
  if (symbol_ids && symbol_count > 0)
  {
    s.symbol_ids.assign(symbol_ids, symbol_ids + symbol_count);
  }
  if (payload && payload_len > 0)
  {
    s.payload = std::string_view(reinterpret_cast<const char*>(payload), payload_len);
  }
  rec->writeSignal(s);
}

extern "C" void flox_run_recorder_write_order_event(FloxRunRecorderHandle handle,
                                                    int64_t run_ts_ns, int64_t feed_ts_ns,
                                                    uint64_t order_id, uint64_t parent_signal_id,
                                                    int64_t price_raw, int64_t qty_raw,
                                                    uint32_t symbol_id, uint8_t event_kind,
                                                    uint8_t side, uint8_t order_type,
                                                    uint32_t flags,
                                                    const char* reason, size_t reason_len)
{
  auto* rec = asRecorder(handle);
  if (rec == nullptr)
  {
    return;
  }
  flox::run::OrderEventView e;
  e.run_ts_ns = run_ts_ns;
  e.feed_ts_ns = feed_ts_ns;
  e.order_id = order_id;
  e.parent_signal_id = parent_signal_id;
  e.price_raw = price_raw;
  e.qty_raw = qty_raw;
  e.symbol_id = symbol_id;
  e.event_kind = static_cast<flox::run::OrderEventKind>(event_kind);
  e.side = side;
  e.order_type = order_type;
  e.flags = flags;
  if (reason && reason_len > 0)
  {
    e.reason = std::string_view(reason, reason_len);
  }
  rec->writeOrderEvent(e);
}

extern "C" void flox_run_recorder_write_fill(FloxRunRecorderHandle handle,
                                             int64_t run_ts_ns, int64_t feed_ts_ns,
                                             uint64_t order_id, uint64_t fill_id,
                                             int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                                             uint32_t symbol_id, uint8_t side, uint8_t liquidity)
{
  auto* rec = asRecorder(handle);
  if (rec == nullptr)
  {
    return;
  }
  flox::run::FillView f;
  f.run_ts_ns = run_ts_ns;
  f.feed_ts_ns = feed_ts_ns;
  f.order_id = order_id;
  f.fill_id = fill_id;
  f.price_raw = price_raw;
  f.qty_raw = qty_raw;
  f.fee_raw = fee_raw;
  f.symbol_id = symbol_id;
  f.side = side;
  f.liquidity = static_cast<flox::run::FillLiquidity>(liquidity);
  rec->writeFill(f);
}

extern "C" void flox_run_recorder_close(FloxRunRecorderHandle handle)
{
  if (auto* rec = asRecorder(handle))
  {
    rec->close();
  }
}

extern "C" FloxRunReaderHandle flox_run_reader_open(const char* path)
{
  if (path == nullptr)
  {
    return nullptr;
  }
  try
  {
    auto state = std::make_unique<RunReaderState>();
    state->reader = std::make_unique<flox::run::TraceReader>(path);
    state->signals = state->reader->readAllSignals();
    state->orders = state->reader->readAllOrderEvents();
    state->fills = state->reader->readAllFills();
    return state.release();
  }
  catch (...)
  {
    return nullptr;
  }
}

extern "C" void flox_run_reader_close(FloxRunReaderHandle handle)
{
  delete asReader(handle);
}

extern "C" uint64_t flox_run_reader_strategy_id(FloxRunReaderHandle handle, char* out, uint64_t max_bytes)
{
  auto* state = asReader(handle);
  if (state == nullptr)
  {
    return 0;
  }
  return copyStringTo(state->reader->manifest().strategy_id, out, max_bytes);
}

extern "C" uint64_t flox_run_reader_strategy_hash(FloxRunReaderHandle handle, char* out, uint64_t max_bytes)
{
  auto* state = asReader(handle);
  if (state == nullptr)
  {
    return 0;
  }
  return copyStringTo(state->reader->manifest().strategy_hash, out, max_bytes);
}

extern "C" int64_t flox_run_reader_run_started_ns(FloxRunReaderHandle handle)
{
  auto* state = asReader(handle);
  return state ? state->reader->manifest().run_started_ns : 0;
}

extern "C" int64_t flox_run_reader_run_ended_ns(FloxRunReaderHandle handle)
{
  auto* state = asReader(handle);
  return state ? state->reader->manifest().run_ended_ns : 0;
}

extern "C" uint64_t flox_run_reader_tape_ref_count(FloxRunReaderHandle handle)
{
  auto* state = asReader(handle);
  return state ? state->reader->manifest().tape_refs.size() : 0;
}

extern "C" uint64_t flox_run_reader_tape_ref_path(FloxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes)
{
  auto* state = asReader(handle);
  if (state == nullptr)
  {
    return 0;
  }
  const auto& refs = state->reader->manifest().tape_refs;
  if (index >= refs.size())
  {
    return 0;
  }
  return copyStringTo(refs[index].path, out, max_bytes);
}

extern "C" uint64_t flox_run_reader_signal_count(FloxRunReaderHandle handle)
{
  auto* state = asReader(handle);
  return state ? state->signals.size() : 0;
}

extern "C" uint64_t flox_run_reader_order_event_count(FloxRunReaderHandle handle)
{
  auto* state = asReader(handle);
  return state ? state->orders.size() : 0;
}

extern "C" uint64_t flox_run_reader_fill_count(FloxRunReaderHandle handle)
{
  auto* state = asReader(handle);
  return state ? state->fills.size() : 0;
}

extern "C" void flox_run_reader_signal_header(FloxRunReaderHandle handle, uint64_t index,
                                              int64_t* out_run_ts, int64_t* out_feed_ts,
                                              uint32_t* out_signal_id, uint32_t* out_flags,
                                              int64_t* out_strength_raw,
                                              uint64_t* out_name_len, uint64_t* out_symbol_count,
                                              uint64_t* out_payload_len)
{
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return;
  }
  const auto& s = state->signals[index];
  if (out_run_ts)
  {
    *out_run_ts = s.run_ts_ns;
  }
  if (out_feed_ts)
  {
    *out_feed_ts = s.feed_ts_ns;
  }
  if (out_signal_id)
  {
    *out_signal_id = s.signal_id;
  }
  if (out_flags)
  {
    *out_flags = s.flags;
  }
  if (out_strength_raw)
  {
    *out_strength_raw = s.strength_raw;
  }
  if (out_name_len)
  {
    *out_name_len = s.name.size();
  }
  if (out_symbol_count)
  {
    *out_symbol_count = s.symbol_ids.size();
  }
  if (out_payload_len)
  {
    *out_payload_len = s.payload.size();
  }
}

extern "C" uint64_t flox_run_reader_signal_name(FloxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes)
{
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return 0;
  }
  return copyStringTo(state->signals[index].name, out, max_bytes);
}

extern "C" uint64_t flox_run_reader_signal_symbol_ids(FloxRunReaderHandle handle, uint64_t index, uint32_t* out, uint64_t max_entries)
{
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return 0;
  }
  const auto& ids = state->signals[index].symbol_ids;
  if (out == nullptr || max_entries == 0)
  {
    return ids.size();
  }
  uint64_t n = std::min(static_cast<uint64_t>(ids.size()), max_entries);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i] = ids[i];
  }
  return n;
}

extern "C" uint64_t flox_run_reader_signal_payload(FloxRunReaderHandle handle, uint64_t index, uint8_t* out, uint64_t max_bytes)
{
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return 0;
  }
  const auto& p = state->signals[index].payload;
  if (out == nullptr || max_bytes == 0)
  {
    return p.size();
  }
  uint64_t n = std::min(static_cast<uint64_t>(p.size()), max_bytes);
  std::memcpy(out, p.data(), n);
  return n;
}

extern "C" void flox_run_reader_order_event_header(FloxRunReaderHandle handle, uint64_t index,
                                                   int64_t* out_run_ts, int64_t* out_feed_ts,
                                                   uint64_t* out_order_id, uint64_t* out_parent_signal_id,
                                                   int64_t* out_price_raw, int64_t* out_qty_raw,
                                                   uint32_t* out_symbol_id, uint8_t* out_event_kind,
                                                   uint8_t* out_side, uint8_t* out_order_type,
                                                   uint32_t* out_flags, uint64_t* out_reason_len)
{
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->orders.size())
  {
    return;
  }
  const auto& e = state->orders[index];
  if (out_run_ts)
  {
    *out_run_ts = e.run_ts_ns;
  }
  if (out_feed_ts)
  {
    *out_feed_ts = e.feed_ts_ns;
  }
  if (out_order_id)
  {
    *out_order_id = e.order_id;
  }
  if (out_parent_signal_id)
  {
    *out_parent_signal_id = e.parent_signal_id;
  }
  if (out_price_raw)
  {
    *out_price_raw = e.price_raw;
  }
  if (out_qty_raw)
  {
    *out_qty_raw = e.qty_raw;
  }
  if (out_symbol_id)
  {
    *out_symbol_id = e.symbol_id;
  }
  if (out_event_kind)
  {
    *out_event_kind = static_cast<uint8_t>(e.event_kind);
  }
  if (out_side)
  {
    *out_side = e.side;
  }
  if (out_order_type)
  {
    *out_order_type = e.order_type;
  }
  if (out_flags)
  {
    *out_flags = e.flags;
  }
  if (out_reason_len)
  {
    *out_reason_len = e.reason.size();
  }
}

extern "C" uint64_t flox_run_reader_order_event_reason(FloxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes)
{
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->orders.size())
  {
    return 0;
  }
  return copyStringTo(state->orders[index].reason, out, max_bytes);
}

extern "C" void flox_run_reader_fill(FloxRunReaderHandle handle, uint64_t index,
                                     int64_t* out_run_ts, int64_t* out_feed_ts,
                                     uint64_t* out_order_id, uint64_t* out_fill_id,
                                     int64_t* out_price_raw, int64_t* out_qty_raw, int64_t* out_fee_raw,
                                     uint32_t* out_symbol_id, uint8_t* out_side, uint8_t* out_liquidity)
{
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->fills.size())
  {
    return;
  }
  const auto& f = state->fills[index];
  if (out_run_ts)
  {
    *out_run_ts = f.run_ts_ns;
  }
  if (out_feed_ts)
  {
    *out_feed_ts = f.feed_ts_ns;
  }
  if (out_order_id)
  {
    *out_order_id = f.order_id;
  }
  if (out_fill_id)
  {
    *out_fill_id = f.fill_id;
  }
  if (out_price_raw)
  {
    *out_price_raw = f.price_raw;
  }
  if (out_qty_raw)
  {
    *out_qty_raw = f.qty_raw;
  }
  if (out_fee_raw)
  {
    *out_fee_raw = f.fee_raw;
  }
  if (out_symbol_id)
  {
    *out_symbol_id = f.symbol_id;
  }
  if (out_side)
  {
    *out_side = f.side;
  }
  if (out_liquidity)
  {
    *out_liquidity = f.liquidity;
  }
}

namespace
{
inline flox::testing::BarDispatchRecorder* toBarDispatchRecorder(FloxBarDispatchRecorderHandle h)
{
  return static_cast<flox::testing::BarDispatchRecorder*>(h);
}
}  // namespace

extern "C" FloxBarDispatchRecorderHandle flox_bar_dispatch_recorder_create(void)
{
  return new flox::testing::BarDispatchRecorder();
}

extern "C" void flox_bar_dispatch_recorder_destroy(FloxBarDispatchRecorderHandle h)
{
  delete toBarDispatchRecorder(h);
}

extern "C" uint32_t flox_bar_dispatch_recorder_add_time_seconds(FloxBarDispatchRecorderHandle h,
                                                                uint32_t seconds)
{
  return static_cast<uint32_t>(toBarDispatchRecorder(h)->addTimeIntervalSeconds(seconds));
}

extern "C" void flox_bar_dispatch_recorder_on_trade(FloxBarDispatchRecorderHandle h,
                                                    uint32_t symbol, double price, double qty,
                                                    int64_t ts_ns)
{
  toBarDispatchRecorder(h)->onTrade(symbol, price, qty, ts_ns);
}

extern "C" void flox_bar_dispatch_recorder_finalize(FloxBarDispatchRecorderHandle h)
{
  toBarDispatchRecorder(h)->finalize();
}

extern "C" uint32_t flox_bar_dispatch_recorder_count(FloxBarDispatchRecorderHandle h)
{
  return static_cast<uint32_t>(toBarDispatchRecorder(h)->count());
}

extern "C" uint8_t flox_bar_dispatch_recorder_type_at(FloxBarDispatchRecorderHandle h,
                                                      uint32_t index)
{
  return toBarDispatchRecorder(h)->typeAt(index);
}

extern "C" uint64_t flox_bar_dispatch_recorder_param_at(FloxBarDispatchRecorderHandle h,
                                                        uint32_t index)
{
  return toBarDispatchRecorder(h)->paramAt(index);
}

// ============================================================
// Streaming tape aggregators (W14-T019)
// ============================================================

namespace
{

// Tagged holder. The C ABI exposes a single FloxAggregatorHandle
// (void*), but each result accessor needs to know the concrete type
// to do a typed static_cast. The `kind` tag is checked on every
// `*_read_result` entry; mismatched kind → return 0 rows so a buggy
// binding can't read garbage past the end of a smaller Row struct.
struct AggregatorHolder
{
  enum Kind : uint8_t
  {
    KIND_EVENT_TYPE_STATS,
    KIND_BIN_COUNT,
    KIND_VOLUME_BIN,
    KIND_OHLC_BIN,
    KIND_PEAK,
    KIND_QUANTILE,
  };
  Kind kind;
  std::unique_ptr<replay::IAggregator> impl;

  template <typename T>
  T* as(Kind expected)
  {
    if (kind != expected)
    {
      return nullptr;
    }
    return static_cast<T*>(impl.get());
  }
};

inline AggregatorHolder* toAgg(FloxAggregatorHandle h)
{
  return static_cast<AggregatorHolder*>(h);
}

inline replay::AggregatorEventFilter toAggFilter(FloxAggregatorEventFilter f)
{
  switch (f)
  {
    case FLOX_AGG_FILTER_TRADES:
      return replay::AggregatorEventFilter::Trades;
    case FLOX_AGG_FILTER_BOOKS_ONLY:
      return replay::AggregatorEventFilter::BooksOnly;
    case FLOX_AGG_FILTER_BOTH:
    default:
      return replay::AggregatorEventFilter::Both;
  }
}

inline std::vector<uint32_t> copySymbolFilter(const uint32_t* sf, uint32_t count)
{
  if (sf == nullptr || count == 0)
  {
    return {};
  }
  return std::vector<uint32_t>(sf, sf + count);
}

}  // namespace

extern "C" FloxAggregatorHandle flox_event_type_stats_aggregator_create(
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_EVENT_TYPE_STATS,
      std::make_unique<replay::EventTypeStatsAggregator>(
          toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
}

extern "C" FloxAggregatorHandle flox_bin_count_aggregator_create(
    int64_t bucket_ns, uint8_t by_side, uint8_t by_symbol,
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_BIN_COUNT,
      std::make_unique<replay::BinCountAggregator>(
          bucket_ns, by_side != 0, by_symbol != 0, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
}

extern "C" FloxAggregatorHandle flox_volume_bin_aggregator_create(
    int64_t bucket_ns, uint8_t by_side, uint8_t by_symbol,
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_VOLUME_BIN,
      std::make_unique<replay::VolumeBinAggregator>(
          bucket_ns, by_side != 0, by_symbol != 0, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
}

extern "C" FloxAggregatorHandle flox_ohlc_bin_aggregator_create(
    int64_t bucket_ns, uint8_t by_symbol,
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_OHLC_BIN,
      std::make_unique<replay::OHLCBinAggregator>(
          bucket_ns, by_symbol != 0, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
}

extern "C" FloxAggregatorHandle flox_peak_aggregator_create(
    const int64_t* window_ns_list, uint32_t window_count, uint32_t top_n,
    uint32_t oversample_factor, FloxAggregatorEventFilter event_filter,
    const uint32_t* symbol_filter, uint32_t symbol_filter_count)
{
  std::vector<int64_t> windows;
  if (window_ns_list != nullptr && window_count > 0)
  {
    windows.assign(window_ns_list, window_ns_list + window_count);
  }
  // 0 → engine default (100). PeakAggregator's ctor maxes the factor
  // at 1 internally so a 0 here would collapse the heap to top_n
  // candidates only — pass 100 to preserve the spec'd default.
  const std::size_t oversample =
      oversample_factor == 0 ? std::size_t{100} : std::size_t{oversample_factor};
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_PEAK,
      std::make_unique<replay::PeakAggregator>(
          std::move(windows), top_n, oversample, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
}

extern "C" FloxAggregatorHandle flox_quantile_aggregator_create(
    const int64_t* window_ns_list, uint32_t window_count,
    const double* quantiles, uint32_t quantile_count,
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  std::vector<int64_t> windows;
  if (window_ns_list != nullptr && window_count > 0)
  {
    windows.assign(window_ns_list, window_ns_list + window_count);
  }
  std::vector<double> qs;
  if (quantiles != nullptr && quantile_count > 0)
  {
    qs.assign(quantiles, quantiles + quantile_count);
  }
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_QUANTILE,
      std::make_unique<replay::QuantileAggregator>(
          std::move(windows), std::move(qs), toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
}

extern "C" void flox_aggregator_destroy(FloxAggregatorHandle h)
{
  delete toAgg(h);
}

extern "C" void flox_data_reader_set_progress_callback(
    FloxDataReaderHandle reader, FloxProgressCallback cb, void* user_data,
    uint32_t interval_ms)
{
  if (reader == nullptr)
  {
    return;
  }
  auto* r = static_cast<replay::BinaryLogReader*>(reader);
  if (cb == nullptr)
  {
    r->clearProgressCallback();
    return;
  }
  std::chrono::milliseconds interval{interval_ms == 0 ? 1000u : interval_ms};
  r->setProgressCallback(
      [cb, user_data](double pct, int64_t cursor_ts_ns) -> bool
      { return cb(user_data, pct, cursor_ts_ns) != 0; },
      interval);
}

extern "C" void flox_data_reader_clear_progress_callback(
    FloxDataReaderHandle reader)
{
  if (reader == nullptr)
  {
    return;
  }
  static_cast<replay::BinaryLogReader*>(reader)->clearProgressCallback();
}

extern "C" uint8_t flox_data_reader_run(FloxDataReaderHandle reader,
                                        FloxAggregatorHandle* aggregators,
                                        uint32_t aggregator_count,
                                        uint32_t n_threads)
{
  if (reader == nullptr)
  {
    return 0;
  }
  std::vector<replay::IAggregator*> raw;
  if (aggregators != nullptr && aggregator_count > 0)
  {
    raw.reserve(aggregator_count);
    for (uint32_t i = 0; i < aggregator_count; ++i)
    {
      auto* holder = toAgg(aggregators[i]);
      raw.push_back(holder != nullptr ? holder->impl.get() : nullptr);
    }
  }
  auto* r = static_cast<replay::BinaryLogReader*>(reader);
  const std::size_t nt = n_threads == 0 ? std::size_t{1} : std::size_t{n_threads};
  return r->run(raw, nt) ? 1 : 0;
}

extern "C" uint8_t flox_merged_tape_reader_run(FloxMergedTapeReaderHandle reader,
                                               FloxAggregatorHandle* aggregators,
                                               uint32_t aggregator_count,
                                               uint32_t /*n_threads*/)
{
  // n_threads reserved for future; MergedTapeReader::run is single-
  // threaded for now (per-instance symbol rekey would not align
  // across worker partitions).
  if (reader == nullptr)
  {
    return 0;
  }
  std::vector<replay::IAggregator*> raw;
  if (aggregators != nullptr && aggregator_count > 0)
  {
    raw.reserve(aggregator_count);
    for (uint32_t i = 0; i < aggregator_count; ++i)
    {
      auto* holder = toAgg(aggregators[i]);
      raw.push_back(holder != nullptr ? holder->impl.get() : nullptr);
    }
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(reader);
  if (!impl || !impl->reader)
  {
    return 0;
  }
  return impl->reader->run(raw) ? 1 : 0;
}

extern "C" uint32_t flox_event_type_stats_read_result(FloxAggregatorHandle h,
                                                      FloxEventTypeStatsRow* rows_out,
                                                      uint32_t max_rows)
{
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::EventTypeStatsAggregator>(
      AggregatorHolder::KIND_EVENT_TYPE_STATS);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].trades = rows[i].trades;
    rows_out[i].book_snapshots = rows[i].book_snapshots;
    rows_out[i].book_deltas = rows[i].book_deltas;
  }
  return n;
}

extern "C" uint32_t flox_bin_count_read_result(FloxAggregatorHandle h,
                                               FloxBinCountRow* rows_out,
                                               uint32_t max_rows)
{
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::BinCountAggregator>(AggregatorHolder::KIND_BIN_COUNT);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].side = rows[i].side;
    rows_out[i].count = rows[i].count;
  }
  return n;
}

extern "C" uint32_t flox_volume_bin_read_result(FloxAggregatorHandle h,
                                                FloxVolumeBinRow* rows_out,
                                                uint32_t max_rows)
{
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::VolumeBinAggregator>(AggregatorHolder::KIND_VOLUME_BIN);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].side = rows[i].side;
    rows_out[i].qty_raw = rows[i].qty_raw;
  }
  return n;
}

extern "C" uint32_t flox_ohlc_bin_read_result(FloxAggregatorHandle h,
                                              FloxOHLCBinRow* rows_out,
                                              uint32_t max_rows)
{
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::OHLCBinAggregator>(AggregatorHolder::KIND_OHLC_BIN);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].open_raw = rows[i].open_raw;
    rows_out[i].high_raw = rows[i].high_raw;
    rows_out[i].low_raw = rows[i].low_raw;
    rows_out[i].close_raw = rows[i].close_raw;
  }
  return n;
}

extern "C" uint32_t flox_peak_read_result(FloxAggregatorHandle h,
                                          FloxPeakRow* rows_out, uint32_t max_rows)
{
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::PeakAggregator>(AggregatorHolder::KIND_PEAK);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].window_ns = rows[i].window_ns;
    rows_out[i].count = rows[i].count;
    rows_out[i].start_ns = rows[i].start_ns;
  }
  return n;
}

extern "C" uint32_t flox_quantile_read_result(FloxAggregatorHandle h,
                                              FloxQuantileRow* rows_out,
                                              uint32_t max_rows)
{
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::QuantileAggregator>(AggregatorHolder::KIND_QUANTILE);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].window_ns = rows[i].window_ns;
    rows_out[i].quantile = rows[i].quantile;
    rows_out[i].count = rows[i].count;
  }
  return n;
}

// ============================================================
// Live queue position estimator
// ============================================================

#include "flox/execution/live_queue_position_estimator.h"

namespace
{
inline flox::LiveQueuePositionEstimator* toLiveQ(FloxLiveQueuePositionHandle h)
{
  return static_cast<flox::LiveQueuePositionEstimator*>(h);
}
}  // namespace

extern "C" FloxLiveQueuePositionHandle flox_live_queue_position_create(void)
{
  return new flox::LiveQueuePositionEstimator();
}

extern "C" void flox_live_queue_position_destroy(FloxLiveQueuePositionHandle h)
{
  delete toLiveQ(h);
}

extern "C" void flox_live_queue_position_set_confidence_half_life_ns(
    FloxLiveQueuePositionHandle h, int64_t half_life_ns)
{
  toLiveQ(h)->setConfidenceHalfLifeNs(half_life_ns);
}

extern "C" void flox_live_queue_position_set_shrink_factor(
    FloxLiveQueuePositionHandle h, double factor)
{
  toLiveQ(h)->setShrinkAttributionFactor(factor);
}

extern "C" void flox_live_queue_position_on_order_placed(
    FloxLiveQueuePositionHandle h, uint32_t symbol, uint8_t side, int64_t price_raw,
    uint64_t order_id, int64_t order_qty_raw, int64_t level_qty_raw, int64_t ts_ns)
{
  toLiveQ(h)->onOrderPlaced(symbol, static_cast<flox::Side>(side),
                            flox::Price::fromRaw(price_raw), order_id,
                            flox::Quantity::fromRaw(order_qty_raw),
                            flox::Quantity::fromRaw(level_qty_raw), ts_ns);
}

extern "C" void flox_live_queue_position_on_order_cancelled(
    FloxLiveQueuePositionHandle h, uint64_t order_id, int64_t ts_ns)
{
  toLiveQ(h)->onOrderCancelled(order_id, ts_ns);
}

extern "C" void flox_live_queue_position_on_order_filled(
    FloxLiveQueuePositionHandle h, uint64_t order_id, int64_t cumulative_fill_raw,
    int64_t ts_ns)
{
  toLiveQ(h)->onOrderFilled(order_id, flox::Quantity::fromRaw(cumulative_fill_raw), ts_ns);
}

extern "C" void flox_live_queue_position_on_trade(FloxLiveQueuePositionHandle h,
                                                  uint32_t symbol, int64_t price_raw,
                                                  int64_t qty_raw, int64_t ts_ns)
{
  toLiveQ(h)->onTrade(symbol, flox::Price::fromRaw(price_raw),
                      flox::Quantity::fromRaw(qty_raw), ts_ns);
}

extern "C" void flox_live_queue_position_on_trade_with_flag(
    FloxLiveQueuePositionHandle h, uint32_t symbol, int64_t price_raw, int64_t qty_raw,
    int64_t ts_ns, uint8_t is_hidden)
{
  toLiveQ(h)->onTradeWithFlag(symbol, flox::Price::fromRaw(price_raw),
                              flox::Quantity::fromRaw(qty_raw), ts_ns, is_hidden != 0);
}

extern "C" void flox_live_queue_position_set_hidden_order_policy(
    FloxLiveQueuePositionHandle h, uint8_t policy)
{
  toLiveQ(h)->setHiddenOrderPolicy(static_cast<flox::HiddenOrderPolicy>(policy));
}

extern "C" void flox_live_queue_position_on_level_update(
    FloxLiveQueuePositionHandle h, uint32_t symbol, uint8_t side, int64_t price_raw,
    int64_t new_qty_raw, int64_t ts_ns)
{
  toLiveQ(h)->onLevelUpdate(symbol, static_cast<flox::Side>(side),
                            flox::Price::fromRaw(price_raw),
                            flox::Quantity::fromRaw(new_qty_raw), ts_ns);
}

extern "C" uint8_t flox_live_queue_position_snapshot(FloxLiveQueuePositionHandle h,
                                                     uint64_t order_id, int64_t now_ns,
                                                     int64_t* out_slots)
{
  auto snap = toLiveQ(h)->snapshot(order_id, now_ns);
  if (!snap.has_value() || out_slots == nullptr)
  {
    return 0;
  }
  out_slots[0] = static_cast<int64_t>(snap->orderId);
  out_slots[1] = snap->queueAheadEst.raw();
  out_slots[2] = snap->total.raw();
  out_slots[3] = snap->lastUpdateNs;
  double conf = snap->confidence;
  std::memcpy(&out_slots[4], &conf, sizeof(double));
  out_slots[5] = snap->hiddenVolumeSeen.raw();
  return 1;
}

extern "C" uint32_t flox_live_queue_position_tracked_count(FloxLiveQueuePositionHandle h)
{
  return static_cast<uint32_t>(toLiveQ(h)->trackedOrderCount());
}
