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
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/ohlcv_replay_source.h"
#include "flox/replay/tape_diff.h"
#include "flox/report/heatmap_html.h"
#include "flox/risk/portfolio_risk.h"
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

void flox_risk_manager_destroy(FloxRiskManagerHandle rm)
{
  delete static_cast<FloxRiskManagerImpl*>(rm);
}

FloxKillSwitchHandle flox_kill_switch_create(FloxKillSwitchCallbacks callbacks)
{
  auto* ks = new FloxKillSwitchImpl{callbacks};
  return static_cast<FloxKillSwitchHandle>(ks);
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

void flox_order_validator_destroy(FloxOrderValidatorHandle ov)
{
  delete static_cast<FloxOrderValidatorImpl*>(ov);
}

FloxPnLTrackerHandle flox_pnl_tracker_create(FloxPnLTrackerCallbacks callbacks)
{
  return static_cast<FloxPnLTrackerHandle>(new FloxPnLTrackerImpl{callbacks});
}

void flox_pnl_tracker_destroy(FloxPnLTrackerHandle tracker)
{
  delete static_cast<FloxPnLTrackerImpl*>(tracker);
}

FloxStorageSinkHandle flox_storage_sink_create(FloxStorageSinkCallbacks callbacks)
{
  return static_cast<FloxStorageSinkHandle>(new FloxStorageSinkImpl{callbacks});
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

void flox_market_data_recorder_destroy(FloxMarketDataRecorderHandle recorder)
{
  delete static_cast<FloxMarketDataRecorderImpl*>(recorder);
}

FloxReplaySourceHandle flox_replay_source_create(FloxReplaySourceCallbacks callbacks)
{
  return static_cast<FloxReplaySourceHandle>(
      new FloxReplaySourceImpl{callbacks});
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

void flox_execution_listener_destroy(FloxExecutionListenerHandle listener)
{
  delete static_cast<FloxExecutionListenerImpl*>(listener);
}

FloxExecutorHandle flox_executor_create(FloxExecutorCallbacks callbacks)
{
  return static_cast<FloxExecutorHandle>(new FloxExecutorImpl{callbacks});
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

void flox_backtest_runner_set_executor(FloxBacktestRunnerHandle h,
                                       FloxExecutorHandle executor)
{
  toBacktestRunner(h)->setExecutor(
      static_cast<capi_impl::FloxExecutorImpl*>(executor));
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
