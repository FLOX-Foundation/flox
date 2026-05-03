/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// flox_capi_spec.hpp — IDL spec for the FLOX C API.
//
// Read at code-generation time by tools/codegen. NOT compiled into the engine
// (no .cpp includes this file; the engine's hand-written C API implementation
// in src/capi/flox_capi.cpp is the runtime side).
//
// Each annotated declaration here corresponds to one symbol in the generated
// flox_capi.h. Sections are formed by the `group=` annotation argument.
//
// To regenerate the C header from this spec:
//   tools/codegen/.venv/bin/python -m flox_codegen.cli emit-capi \\
//     --spec include/flox/capi/flox_capi_spec.hpp \\
//     --out tools/codegen/golden/slice_capi.h
//
// The CI gate (.github/workflows/codegen.yml) runs codegen and diffs against
// the committed golden output; signature mismatches against the live
// flox_capi.h block the merge.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "flox/capi/flox_export.h"

#ifdef __cplusplus
extern "C"
{
#endif

  // ============================================================
  // Opaque handles
  // ============================================================

  typedef void* FloxStrategyHandle;
  typedef void* FloxRegistryHandle;

  // ============================================================
  // Flat event structs
  // ============================================================

  typedef struct
  {
    uint32_t symbol;
    int64_t price_raw;
    int64_t quantity_raw;
    uint8_t is_buy;
    int64_t exchange_ts_ns;
  } FloxTradeData;

  typedef struct
  {
    int64_t bid_price_raw;
    int64_t bid_qty_raw;
    int64_t ask_price_raw;
    int64_t ask_qty_raw;
    int64_t mid_raw;
    int64_t spread_raw;
  } FloxBookSnapshot;

  typedef struct
  {
    uint32_t symbol;
    int64_t exchange_ts_ns;
    FloxBookSnapshot snapshot;
  } FloxBookData;

  typedef struct
  {
    uint32_t symbol;
    uint8_t bar_type;
    uint8_t close_reason;
    uint8_t _pad[2];
    uint64_t bar_type_param;
    int64_t open_raw;
    int64_t high_raw;
    int64_t low_raw;
    int64_t close_raw;
    int64_t volume_raw;
    int64_t buy_volume_raw;
    int64_t trade_count_raw;
    int64_t start_time_ns;
    int64_t end_time_ns;
  } FloxBarData;

  typedef struct
  {
    uint32_t symbol_id;
    int64_t position_raw;
    int64_t avg_entry_price_raw;
    int64_t last_trade_price_raw;
    int64_t last_update_ns;
    FloxBookSnapshot book;
  } FloxSymbolContext;

  // ============================================================
  // Callback function pointer types
  // ============================================================

  typedef void (*FloxOnTradeCallback)(void* user_data, const FloxSymbolContext* ctx,
                                      const FloxTradeData* trade);
  typedef void (*FloxOnBookCallback)(void* user_data, const FloxSymbolContext* ctx,
                                     const FloxBookData* book);
  typedef void (*FloxOnBarCallback)(void* user_data, const FloxSymbolContext* ctx,
                                    const FloxBarData* bar);
  typedef void (*FloxOnStartCallback)(void* user_data);
  typedef void (*FloxOnStopCallback)(void* user_data);

  typedef struct
  {
    FloxOnTradeCallback on_trade;
    FloxOnBookCallback on_book;
    FloxOnBarCallback on_bar;
    FloxOnStartCallback on_start;
    FloxOnStopCallback on_stop;
    void* user_data;
  } FloxStrategyCallbacks;

  // ============================================================
  // Symbol registry
  // ============================================================

  FLOX_EXPORT(group = "registry")
  FloxRegistryHandle flox_registry_create(void);

  FLOX_EXPORT(group = "registry")
  void flox_registry_destroy(FloxRegistryHandle registry);

  FLOX_EXPORT(group = "registry")
  uint32_t flox_registry_add_symbol(FloxRegistryHandle registry, const char* exchange,
                                    const char* name, double tick_size);

  FLOX_EXPORT(group = "registry")
  uint8_t flox_registry_get_symbol_id(FloxRegistryHandle registry, const char* exchange,
                                      const char* name, uint32_t* id_out);

  FLOX_EXPORT(group = "registry")
  uint8_t flox_registry_get_symbol_name(FloxRegistryHandle registry, uint32_t symbol_id,
                                        char* exchange_out, size_t exchange_len, char* name_out,
                                        size_t name_len);

  FLOX_EXPORT(group = "registry")
  uint32_t flox_registry_symbol_count(FloxRegistryHandle registry);

  // ============================================================
  // Strategy lifecycle
  // ============================================================

  FLOX_EXPORT(group = "strategy")
  FloxStrategyHandle flox_strategy_create(uint32_t id, const uint32_t* symbols,
                                          uint32_t num_symbols, FloxRegistryHandle registry,
                                          FloxStrategyCallbacks callbacks);

  FLOX_EXPORT(group = "strategy")
  void flox_strategy_destroy(FloxStrategyHandle strategy);

  // ============================================================
  // Signal emission (slice: market buy/sell only; full set in T014)
  // ============================================================

  FLOX_EXPORT(group = "signal")
  uint64_t flox_emit_market_buy(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw);

  FLOX_EXPORT(group = "signal")
  uint64_t flox_emit_market_sell(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw);

  // ============================================================
  // Context queries
  // ============================================================

  FLOX_EXPORT(group = "context")
  int64_t flox_position_raw(FloxStrategyHandle s, uint32_t symbol);

  FLOX_EXPORT(group = "context")
  int64_t flox_last_trade_price_raw(FloxStrategyHandle s, uint32_t symbol);

  FLOX_EXPORT(group = "context")
  int64_t flox_best_bid_raw(FloxStrategyHandle s, uint32_t symbol);

  FLOX_EXPORT(group = "context")
  int64_t flox_best_ask_raw(FloxStrategyHandle s, uint32_t symbol);

  FLOX_EXPORT(group = "context")
  int64_t flox_mid_price_raw(FloxStrategyHandle s, uint32_t symbol);

  FLOX_EXPORT(group = "context")
  void flox_get_symbol_context(FloxStrategyHandle s, uint32_t symbol, FloxSymbolContext* out);

  FLOX_EXPORT(group = "context")
  int32_t flox_get_order_status(FloxStrategyHandle s, uint64_t order_id);

  // ============================================================
  // Fixed-point conversion helpers
  // ============================================================

  FLOX_EXPORT(group = "fixed_point")
  int64_t flox_price_from_double(double value);

  FLOX_EXPORT(group = "fixed_point")
  double flox_price_to_double(int64_t raw);

  FLOX_EXPORT(group = "fixed_point")
  int64_t flox_quantity_from_double(double value);

  FLOX_EXPORT(group = "fixed_point")
  double flox_quantity_to_double(int64_t raw);

  // ============================================================
  // Indicator functions (stateless, array-in/array-out)
  // ============================================================

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_ema(const double* input, size_t len, size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_sma(const double* input, size_t len, size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_rsi(const double* input, size_t len, size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_atr(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_macd(const double* input, size_t len, size_t fast_period,
                           size_t slow_period, size_t signal_period, double* macd_out,
                           double* signal_out, double* hist_out);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_bollinger(const double* input, size_t len, size_t period, double multiplier,
                                double* upper, double* middle, double* lower);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_rma(const double* input, size_t len, size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_dema(const double* input, size_t len, size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_tema(const double* input, size_t len, size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_kama(const double* input, size_t len, size_t period, size_t fast, size_t slow,
                           double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_slope(const double* input, size_t len, size_t length, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_adx(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* adx_out, double* plus_di_out,
                          double* minus_di_out);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_cci(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_stochastic(const double* high, const double* low, const double* close,
                                 size_t len, size_t k_period, size_t d_period, double* k_out,
                                 double* d_out);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_chop(const double* high, const double* low, const double* close, size_t len,
                           size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_obv(const double* close, const double* volume, size_t len, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_vwap(const double* close, const double* volume, size_t len, size_t window,
                           double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_cvd(const double* open, const double* high, const double* low,
                          const double* close, const double* volume, size_t len, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_skewness(const double* input, size_t len, size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_kurtosis(const double* input, size_t len, size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_parkinson_vol(const double* high, const double* low, size_t len,
                                    size_t period, double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_rogers_satchell_vol(const double* open, const double* high, const double* low,
                                          const double* close, size_t len, size_t period,
                                          double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_rolling_zscore(const double* input, size_t len, size_t period,
                                     double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_shannon_entropy(const double* input, size_t len, size_t period, size_t bins,
                                      double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_correlation(const double* x, const double* y, size_t len, size_t period,
                                  double* output);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_adf(const double* input, size_t len, size_t max_lag, const char* regression,
                          double* test_stat_out, double* p_value_out, size_t* used_lag_out);

  FLOX_EXPORT(group = "indicator")
  void flox_indicator_autocorrelation(const double* input, size_t len, size_t window, size_t lag,
                                      double* output);

  // ============================================================
  // Targets (forward-looking labels, batch only)
  // ============================================================

  FLOX_EXPORT(group = "target")
  void flox_target_future_return(const double* close, size_t len, size_t horizon, double* output);

  FLOX_EXPORT(group = "target")
  void flox_target_future_ctc_volatility(const double* close, size_t len, size_t horizon,
                                         double* output);

  FLOX_EXPORT(group = "target")
  void flox_target_future_linear_slope(const double* close, size_t len, size_t horizon,
                                       double* output);

  // ============================================================
  // Statistics
  // ============================================================

  FLOX_EXPORT(group = "stat")
  double flox_stat_correlation(const double* x, const double* y, size_t len);

  FLOX_EXPORT(group = "stat")
  double flox_stat_profit_factor(const double* pnl, size_t len);

  FLOX_EXPORT(group = "stat")
  double flox_stat_win_rate(const double* pnl, size_t len);

#ifdef __cplusplus
}
#endif
