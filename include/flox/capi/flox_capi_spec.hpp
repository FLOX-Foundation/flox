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
// Generated from flox_capi.h via tools/codegen/scripts/import_capi.py.
// After bootstrap, this file is the source of truth — edit here, regenerate
// the C header via tools/codegen/scripts/regenerate.sh.

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
  typedef void* FloxBookHandle;
  typedef void* FloxSimulatedExecutorHandle;
  typedef void* FloxPositionTrackerHandle;
  typedef void* FloxPositionGroupHandle;
  typedef void* FloxOrderTrackerHandle;
  typedef void* FloxOrderJourneyTracerHandle;
  typedef void* FloxFootprintHandle;
  typedef void* FloxVolumeProfileHandle;
  typedef void* FloxMarketProfileHandle;
  typedef void* FloxCompositeBookHandle;

  // ============================================================
  // Flat event structs (C-compatible, no C++ dependencies)
  // ============================================================

  typedef struct
  {
    uint32_t symbol;
    int64_t price_raw;     // Price * 1e8
    int64_t quantity_raw;  // Quantity * 1e8
    uint8_t is_buy;
    int64_t exchange_ts_ns;
  } FloxTradeData;

  typedef struct
  {
    int64_t price_raw;
    int64_t quantity_raw;
  } FloxBookLevel;

  typedef struct
  {
    int64_t bid_price_raw;  // best bid, or 0 if absent
    int64_t bid_qty_raw;
    int64_t ask_price_raw;  // best ask, or 0 if absent
    int64_t ask_qty_raw;
    int64_t mid_raw;     // mid price, or 0
    int64_t spread_raw;  // spread, or 0
  } FloxBookSnapshot;

  typedef struct
  {
    uint32_t symbol;
    int64_t exchange_ts_ns;
    FloxBookSnapshot snapshot;
  } FloxBookData;

  // OHLC bar event. bar_type: 0=Time, 1=Tick, 2=Volume, 3=Renko, 4=Range, 5=HeikinAshi.
  // bar_type_param: interval_ns / tick count / volume threshold depending on type.
  // close_reason: 0=Threshold, 1=Gap, 2=Forced, 3=Warmup.
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

  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol_id;
    uint8_t side;        // 0 BUY, 1 SELL
    uint8_t order_type;  // 0 LIMIT, 1 MARKET, 2 STOP_MARKET, ...
    uint8_t status;      // FloxOrderEventStatus
    uint8_t _pad;
    int64_t fill_qty_raw;
    int64_t fill_price_raw;
    int64_t exchange_ts_ns;
    const char* reject_reason;  // null when status != REJECTED
    int64_t queue_ahead_raw;    // queue position (backtest limit orders only)
    int64_t queue_total_raw;    // total quantity at the order's price level
    // Per-lifecycle-stage timestamps. Zero means the stage has not
    // fired yet for this order.
    int64_t submitted_at_ns;
    int64_t accepted_at_ns;
    int64_t first_fill_at_ns;
    int64_t last_fill_at_ns;
    int64_t canceled_at_ns;
    int64_t rejected_at_ns;
    int64_t triggered_at_ns;
    int64_t expired_at_ns;
    uint8_t is_maker;         // 0 taker, 1 maker; meaningful only for fill statuses
    uint8_t market_position;  // 0 unknown, 1 best, 2 behind_best, 3 mid_spread, 4 level_empty, 5 crossed
    int32_t distance_to_best_ticks;
    uint8_t _pad2[2];
  } FloxOrderEventData;

  typedef void (*FloxOnTradeCallback)(void* user_data, const FloxSymbolContext* ctx,
                                      const FloxTradeData* trade);
  typedef void (*FloxOnBookCallback)(void* user_data, const FloxSymbolContext* ctx,
                                     const FloxBookData* book);
  typedef void (*FloxOnBarCallback)(void* user_data, const FloxSymbolContext* ctx,
                                    const FloxBarData* bar);
  typedef void (*FloxOnFillCallback)(void* user_data, const FloxSymbolContext* ctx,
                                     const FloxOrderEventData* ev);
  typedef void (*FloxOnOrderUpdateCallback)(void* user_data, const FloxSymbolContext* ctx,
                                            const FloxOrderEventData* ev);
  typedef void (*FloxOnStartCallback)(void* user_data);
  typedef void (*FloxOnStopCallback)(void* user_data);

  typedef void (*FloxOnQueuePositionChangeCallback)(void* user_data,
                                                    const FloxSymbolContext* ctx,
                                                    const FloxOrderEventData* ev);
  typedef void (*FloxOnMarketPositionChangeCallback)(void* user_data,
                                                     const FloxSymbolContext* ctx,
                                                     const FloxOrderEventData* ev);

  typedef struct
  {
    FloxOnTradeCallback on_trade;
    FloxOnBookCallback on_book;
    FloxOnBarCallback on_bar;
    FloxOnStartCallback on_start;
    FloxOnStopCallback on_stop;
    FloxOnFillCallback on_fill;
    FloxOnOrderUpdateCallback on_order_update;
    FloxOnQueuePositionChangeCallback on_queue_position_change;
    FloxOnMarketPositionChangeCallback on_market_position_change;
    void* user_data;
  } FloxStrategyCallbacks;

  // ============================================================
  // Symbol registry
  // ============================================================

  FLOX_EXPORT(group = "symbol_registry")
  FloxRegistryHandle flox_registry_create(void);
  FLOX_EXPORT(group = "symbol_registry")
  void flox_registry_destroy(FloxRegistryHandle registry);
  FLOX_EXPORT(group = "symbol_registry")
  uint32_t flox_registry_add_symbol(FloxRegistryHandle registry, const char* exchange,
                                    const char* name, double tick_size);

  // Symbol name resolution
  FLOX_EXPORT(group = "symbol_registry")
  uint8_t flox_registry_get_symbol_id(FloxRegistryHandle registry, const char* exchange,
                                      const char* name, uint32_t* id_out);
  FLOX_EXPORT(group = "symbol_registry")
  uint8_t flox_registry_get_symbol_name(FloxRegistryHandle registry, uint32_t symbol_id,
                                        char* exchange_out, size_t exchange_len, char* name_out,
                                        size_t name_len);
  FLOX_EXPORT(group = "symbol_registry")
  uint32_t flox_registry_symbol_count(FloxRegistryHandle registry);

  // ============================================================
  // Strategy lifecycle
  // ============================================================

  FLOX_EXPORT(group = "strategy_lifecycle")
  FloxStrategyHandle flox_strategy_create(uint32_t id, const uint32_t* symbols,
                                          uint32_t num_symbols, FloxRegistryHandle registry,
                                          FloxStrategyCallbacks callbacks);
  FLOX_EXPORT(group = "strategy_lifecycle")
  void flox_strategy_destroy(FloxStrategyHandle strategy);

  // Atomically replace the strategy's callback set without dropping
  // any subscriptions, in-flight orders, or open WebSocket / gRPC
  // connections. The next dispatched event sees the new callbacks.
  // Lifecycle: on_stop fires on the old user_data before the swap;
  // on_start fires on the new user_data after. Use this to hot-reload
  // strategy logic in production without tearing down the connector.
  FLOX_EXPORT(group = "strategy_lifecycle")
  void flox_strategy_replace_callbacks(FloxStrategyHandle strategy,
                                       FloxStrategyCallbacks callbacks);

  // Pointer-variant constructors for FFIs that cannot pass a struct
  // by value. The `_p` form takes a pointer to the same struct;
  // calling `*p` to read is equivalent to passing the struct directly.
  // These exist alongside the by-value variants so existing
  // pybind11/NAPI/QuickJS callers keep working unchanged. (Used by
  // Codon today.)
  FLOX_EXPORT(group = "strategy_lifecycle")
  FloxStrategyHandle flox_strategy_create_p(uint32_t id, const uint32_t* symbols,
                                            uint32_t num_symbols, FloxRegistryHandle registry,
                                            const FloxStrategyCallbacks* callbacks);
  FLOX_EXPORT(group = "strategy_lifecycle")
  void flox_strategy_replace_callbacks_p(FloxStrategyHandle strategy,
                                         const FloxStrategyCallbacks* callbacks);

  // ============================================================
  // Signal emission (returns OrderId, 0 on failure)
  // ============================================================

  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_market_buy(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_market_sell(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_limit_buy(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                               int64_t qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_limit_sell(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                int64_t qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  void flox_emit_cancel(FloxStrategyHandle s, uint64_t order_id);
  FLOX_EXPORT(group = "signal_emission")
  void flox_emit_cancel_all(FloxStrategyHandle s, uint32_t symbol);
  FLOX_EXPORT(group = "signal_emission")
  void flox_emit_modify(FloxStrategyHandle s, uint64_t order_id, int64_t new_price_raw,
                        int64_t new_qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_stop_market(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                 int64_t trigger_raw, int64_t qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_stop_limit(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_take_profit_market(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                        int64_t trigger_raw, int64_t qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_trailing_stop(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                   int64_t offset_raw, int64_t qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_trailing_stop_percent(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                           int32_t callback_bps, int64_t qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_take_profit_limit(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                       int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_limit_buy_tif(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                   int64_t qty_raw, uint8_t time_in_force);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_limit_sell_tif(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                    int64_t qty_raw, uint8_t time_in_force);
  FLOX_EXPORT(group = "signal_emission")
  uint64_t flox_emit_close_position(FloxStrategyHandle s, uint32_t symbol);

  // ============================================================
  // Context queries
  // ============================================================

  FLOX_EXPORT(group = "context_queries")
  int64_t flox_position_raw(FloxStrategyHandle s, uint32_t symbol);
  FLOX_EXPORT(group = "context_queries")
  int64_t flox_last_trade_price_raw(FloxStrategyHandle s, uint32_t symbol);
  FLOX_EXPORT(group = "context_queries")
  int64_t flox_best_bid_raw(FloxStrategyHandle s, uint32_t symbol);
  FLOX_EXPORT(group = "context_queries")
  int64_t flox_best_ask_raw(FloxStrategyHandle s, uint32_t symbol);
  FLOX_EXPORT(group = "context_queries")
  int64_t flox_mid_price_raw(FloxStrategyHandle s, uint32_t symbol);
  FLOX_EXPORT(group = "context_queries")
  void flox_get_symbol_context(FloxStrategyHandle s, uint32_t symbol, FloxSymbolContext* out);
  FLOX_EXPORT(group = "context_queries")
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

  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_ema(const double* input, size_t len, size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_sma(const double* input, size_t len, size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_rsi(const double* input, size_t len, size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_atr(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_macd(const double* input, size_t len, size_t fast_period,
                           size_t slow_period, size_t signal_period, double* macd_out,
                           double* signal_out, double* hist_out);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_bollinger(const double* input, size_t len, size_t period, double multiplier,
                                double* upper, double* middle, double* lower);

  // Moving average variants
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_rma(const double* input, size_t len, size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_dema(const double* input, size_t len, size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_tema(const double* input, size_t len, size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_kama(const double* input, size_t len, size_t period, size_t fast, size_t slow,
                           double* output);

  // Trend
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_slope(const double* input, size_t len, size_t length, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_adx(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* adx_out, double* plus_di_out,
                          double* minus_di_out);

  // Oscillators
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_cci(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_stochastic(const double* high, const double* low, const double* close,
                                 size_t len, size_t k_period, size_t d_period, double* k_out,
                                 double* d_out);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_chop(const double* high, const double* low, const double* close, size_t len,
                           size_t period, double* output);

  // Volume
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_obv(const double* close, const double* volume, size_t len, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_vwap(const double* close, const double* volume, size_t len, size_t window,
                           double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_cvd(const double* open, const double* high, const double* low,
                          const double* close, const double* volume, size_t len, double* output);

  // Statistical
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_skewness(const double* input, size_t len, size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_kurtosis(const double* input, size_t len, size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_parkinson_vol(const double* high, const double* low, size_t len,
                                    size_t period, double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_rogers_satchell_vol(const double* open, const double* high, const double* low,
                                          const double* close, size_t len, size_t period,
                                          double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_rolling_zscore(const double* input, size_t len, size_t period,
                                     double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_shannon_entropy(const double* input, size_t len, size_t period, size_t bins,
                                      double* output);
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_correlation(const double* x, const double* y, size_t len, size_t period,
                                  double* output);

  // Augmented Dickey-Fuller test. `regression` accepts: "n", "c", "ct".
  // Writes the t-statistic, approximate p-value, and AIC-selected lag.
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_adf(const double* input, size_t len, size_t max_lag, const char* regression,
                          double* test_stat_out, double* p_value_out, size_t* used_lag_out);
  // AutoCorrelation: Pearson correlation between x[t] and x[t-lag] over a
  // rolling window. First valid index is (window + lag - 1).
  FLOX_EXPORT(group = "indicator_functions")
  void flox_indicator_autocorrelation(const double* input, size_t len, size_t window, size_t lag,
                                      double* output);

  // ============================================================
  // Targets (forward-looking labels, batch only)
  //
  // Targets read into the future relative to t. They are intentionally
  // separate from indicators: feeding them into a live update loop is a
  // look-ahead-bias bug.
  // ============================================================

  FLOX_EXPORT(group = "targets")
  void flox_target_future_return(const double* close, size_t len, size_t horizon, double* output);
  FLOX_EXPORT(group = "targets")
  void flox_target_future_ctc_volatility(const double* close, size_t len, size_t horizon,
                                         double* output);
  FLOX_EXPORT(group = "targets")
  void flox_target_future_linear_slope(const double* close, size_t len, size_t horizon,
                                       double* output);

  // ============================================================
  // IndicatorGraph (batch)
  //
  // Compose batch indicators with shared intermediate caching. Nodes are
  // user-supplied callbacks; require() walks the DAG in topological order.
  //
  //   FloxIndicatorGraphHandle g = flox_indicator_graph_create();
  //   flox_indicator_graph_set_bars(g, sym, close, high, low, volume, n);
  //   flox_indicator_graph_add_node(g, "ema50", NULL, 0, ema_fn, ema_state);
  //   const double* out = flox_indicator_graph_require(g, sym, "ema50", &len);
  //   flox_indicator_graph_destroy(g);
  // ============================================================

  typedef void* FloxIndicatorGraphHandle;

  // Compute callback. Output array must be allocated by the callback (or pre-
  // allocated by the user). The host language is responsible for keeping it
  // alive until the next graph call. Returning a pointer with len = 0 signals
  // an empty result.
  typedef const double* (*FloxGraphNodeFn)(void* user_data, FloxIndicatorGraphHandle g,
                                           uint32_t symbol, size_t* out_len);

  FLOX_EXPORT(group = "indicatorgraph")
  FloxIndicatorGraphHandle flox_indicator_graph_create(void);
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_indicator_graph_destroy(FloxIndicatorGraphHandle g);

  // Pass NULL for high/low/volume to default high=low=close and volume=0.
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_indicator_graph_set_bars(FloxIndicatorGraphHandle g, uint32_t symbol,
                                     const double* close, const double* high, const double* low,
                                     const double* volume, size_t len);

  // deps: array of `num_deps` C-string node names (or NULL if num_deps == 0).
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_indicator_graph_add_node(FloxIndicatorGraphHandle g, const char* name,
                                     const char* const* deps, size_t num_deps,
                                     FloxGraphNodeFn fn, void* user_data);

  // Returns a pointer to the cached output for (symbol, name) and writes its
  // length to *len_out. The pointer is owned by the graph and is valid until
  // the next invalidate / destroy call. Returns NULL on error (unknown node,
  // cycle, etc.).
  FLOX_EXPORT(group = "indicatorgraph")
  const double* flox_indicator_graph_require(FloxIndicatorGraphHandle g, uint32_t symbol,
                                             const char* name, size_t* len_out);

  // Same as require but only returns a pointer if the node has already been
  // computed; never triggers compute. Returns NULL if not yet computed.
  FLOX_EXPORT(group = "indicatorgraph")
  const double* flox_indicator_graph_get(FloxIndicatorGraphHandle g, uint32_t symbol,
                                         const char* name, size_t* len_out);

  // Field accessors return cached double arrays for the symbol's bars.
  FLOX_EXPORT(group = "indicatorgraph")
  const double* flox_indicator_graph_close(FloxIndicatorGraphHandle g, uint32_t symbol,
                                           size_t* len_out);
  FLOX_EXPORT(group = "indicatorgraph")
  const double* flox_indicator_graph_high(FloxIndicatorGraphHandle g, uint32_t symbol,
                                          size_t* len_out);
  FLOX_EXPORT(group = "indicatorgraph")
  const double* flox_indicator_graph_low(FloxIndicatorGraphHandle g, uint32_t symbol,
                                         size_t* len_out);
  FLOX_EXPORT(group = "indicatorgraph")
  const double* flox_indicator_graph_volume(FloxIndicatorGraphHandle g, uint32_t symbol,
                                            size_t* len_out);

  FLOX_EXPORT(group = "indicatorgraph")
  void flox_indicator_graph_invalidate(FloxIndicatorGraphHandle g, uint32_t symbol);
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_indicator_graph_invalidate_all(FloxIndicatorGraphHandle g);

  // ── Streaming path on the same handle ────────────────────────────
  // Same graph object, both APIs. step() appends one bar; node cache is
  // invalidated; current() returns the latest value of a named node.

  FLOX_EXPORT(group = "indicatorgraph")
  void flox_indicator_graph_step(FloxIndicatorGraphHandle g, uint32_t symbol, double open,
                                 double high, double low, double close, double volume);
  FLOX_EXPORT(group = "indicatorgraph")
  double flox_indicator_graph_current(FloxIndicatorGraphHandle g, uint32_t symbol,
                                      const char* name);
  FLOX_EXPORT(group = "indicatorgraph")
  uint32_t flox_indicator_graph_bar_count(FloxIndicatorGraphHandle g, uint32_t symbol);
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_indicator_graph_reset(FloxIndicatorGraphHandle g, uint32_t symbol);
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_indicator_graph_reset_all(FloxIndicatorGraphHandle g);

  // ── Deprecated streaming-graph shim ──────────────────────────────
  // The old separate StreamingIndicatorGraph type has been collapsed
  // into IndicatorGraph. These names are now thin forwarders to the new
  // ones and will be removed in a future major version. New code: use
  // flox_indicator_graph_* (one handle, both compute and step).

  typedef FloxIndicatorGraphHandle FloxStreamingGraphHandle;

  FLOX_EXPORT(group = "indicatorgraph")
  FloxStreamingGraphHandle flox_streaming_graph_create(void);
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_streaming_graph_destroy(FloxStreamingGraphHandle sg);
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_streaming_graph_add_node(FloxStreamingGraphHandle sg, const char* name,
                                     const char* const* deps, size_t num_deps,
                                     FloxGraphNodeFn fn, void* user_data);
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_streaming_graph_step(FloxStreamingGraphHandle sg, uint32_t symbol, double open,
                                 double high, double low, double close, double volume);
  FLOX_EXPORT(group = "indicatorgraph")
  double flox_streaming_graph_current(FloxStreamingGraphHandle sg, uint32_t symbol,
                                      const char* name);
  FLOX_EXPORT(group = "indicatorgraph")
  uint32_t flox_streaming_graph_bar_count(FloxStreamingGraphHandle sg, uint32_t symbol);
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_streaming_graph_reset(FloxStreamingGraphHandle sg, uint32_t symbol);
  FLOX_EXPORT(group = "indicatorgraph")
  void flox_streaming_graph_reset_all(FloxStreamingGraphHandle sg);
  FLOX_EXPORT(group = "indicatorgraph")
  const double* flox_streaming_graph_close(FloxStreamingGraphHandle sg, uint32_t symbol,
                                           size_t* len_out);
  FLOX_EXPORT(group = "indicatorgraph")
  const double* flox_streaming_graph_high(FloxStreamingGraphHandle sg, uint32_t symbol,
                                          size_t* len_out);
  FLOX_EXPORT(group = "indicatorgraph")
  const double* flox_streaming_graph_low(FloxStreamingGraphHandle sg, uint32_t symbol,
                                         size_t* len_out);
  FLOX_EXPORT(group = "indicatorgraph")
  const double* flox_streaming_graph_volume(FloxStreamingGraphHandle sg, uint32_t symbol,
                                            size_t* len_out);

  // ============================================================
  // Order book
  // ============================================================

  FLOX_EXPORT(group = "order_book")
  FloxBookHandle flox_book_create(double tick_size);
  FLOX_EXPORT(group = "order_book")
  void flox_book_destroy(FloxBookHandle book);
  FLOX_EXPORT(group = "order_book")
  void flox_book_apply_snapshot(FloxBookHandle book, const double* bid_prices,
                                const double* bid_qtys, size_t bid_len, const double* ask_prices,
                                const double* ask_qtys, size_t ask_len);
  FLOX_EXPORT(group = "order_book")
  void flox_book_apply_delta(FloxBookHandle book, const double* bid_prices, const double* bid_qtys,
                             size_t bid_len, const double* ask_prices, const double* ask_qtys,
                             size_t ask_len);
  FLOX_EXPORT(group = "order_book")
  uint8_t flox_book_best_bid(FloxBookHandle book, double* price_out);
  FLOX_EXPORT(group = "order_book")
  uint8_t flox_book_best_ask(FloxBookHandle book, double* price_out);
  FLOX_EXPORT(group = "order_book")
  uint8_t flox_book_mid(FloxBookHandle book, double* price_out);
  FLOX_EXPORT(group = "order_book")
  uint8_t flox_book_spread(FloxBookHandle book, double* spread_out);
  FLOX_EXPORT(group = "order_book")
  double flox_book_bid_at_price(FloxBookHandle book, double price);
  FLOX_EXPORT(group = "order_book")
  double flox_book_ask_at_price(FloxBookHandle book, double price);
  FLOX_EXPORT(group = "order_book")
  uint8_t flox_book_is_crossed(FloxBookHandle book);
  FLOX_EXPORT(group = "order_book")
  void flox_book_clear(FloxBookHandle book);

  // ============================================================
  // Simulated executor (backtesting)
  // ============================================================

  FLOX_EXPORT(group = "simulated_executor")
  FloxSimulatedExecutorHandle flox_simulated_executor_create(void);
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_destroy(FloxSimulatedExecutorHandle executor);
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_submit_order(FloxSimulatedExecutorHandle executor, uint64_t id, uint8_t side,
                                            double price, double quantity, uint8_t order_type,
                                            uint32_t symbol);

  // Extended submit with explicit TIF / reduce-only / GTD expiry.
  // tif: 0=GTC, 1=IOC, 2=FOK, 3=GTD, 4=POST_ONLY.
  // reduce_only: 1 to enforce reduce-only against the simulator
  // net position; rejects if would open/grow, truncates if would
  // overshoot flat.
  // expires_at_ns: absolute deadline (nanoseconds since epoch) for
  // GTD; ignored for other TIFs.
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_submit_order_ex(FloxSimulatedExecutorHandle executor,
                                               uint64_t id, uint8_t side, double price,
                                               double quantity, uint8_t order_type,
                                               uint32_t symbol, uint8_t tif,
                                               uint8_t reduce_only, int64_t expires_at_ns);

  // Native bracket primitive. Submits the entry leg and arms a
  // take-profit + stop pair on entry fill. The simulator manages
  // the state machine: first child to fill cancels the other;
  // cancelling the bracket cancels every still-live leg.
  // OrderIds for the legs are derived: entry = bracket_id*3+0,
  // take-profit = bracket_id*3+1, stop = bracket_id*3+2.
  // entry_type / tp_type / stop_type follow the OrderType enum
  // (0=LIMIT, 1=MARKET, 2=STOP_MARKET, 3=STOP_LIMIT).
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_submit_bracket(
      FloxSimulatedExecutorHandle executor, uint64_t bracket_id, uint32_t symbol,
      uint8_t entry_side, uint8_t entry_type, double entry_price, double quantity,
      uint8_t tp_side, uint8_t tp_type, double tp_price,
      uint8_t stop_side, uint8_t stop_type, double stop_trigger_price);

  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_cancel_bracket(FloxSimulatedExecutorHandle executor,
                                              uint64_t bracket_id);

  // Read the bracket state. Returns 0=PENDING_ENTRY, 1=ENTRY_FILLED,
  // 2=TP_FILLED, 3=STOP_FILLED, 4=CANCELED. Unknown bracketId
  // returns PENDING_ENTRY.
  FLOX_EXPORT(group = "simulated_executor")
  uint8_t flox_simulated_executor_bracket_state(FloxSimulatedExecutorHandle executor,
                                                uint64_t bracket_id);

  // Bracket child-arm policy (T040). 0=OnFullFill (default), 1=OnPartialFill.
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_set_bracket_child_arm_mode(
      FloxSimulatedExecutorHandle executor, uint8_t mode);

  // Submit a native iceberg order. order_type is forced to ICEBERG
  // internally. `visible_quantity` is the slice exposed to the book;
  // the hidden remainder (total - visible) refreshes automatically
  // as the visible tranche fills. Refresh latency is configured via
  // flox_simulated_executor_set_iceberg_refresh_latency.
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_submit_iceberg(FloxSimulatedExecutorHandle executor,
                                              uint64_t id, uint8_t side, double price,
                                              double total_quantity,
                                              double visible_quantity, uint32_t symbol);

  // Set the default refresh latency in nanoseconds for native
  // iceberg orders submitted after this call. Most venues are
  // instant (0); a few inject 1-2ms.
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_set_iceberg_refresh_latency(
      FloxSimulatedExecutorHandle executor, int64_t latency_ns);

  // Diagnostic: hidden remainder (raw fixed-point) for an iceberg
  // order, or 0 if the order is not an iceberg or has been removed.
  FLOX_EXPORT(group = "simulated_executor")
  int64_t flox_simulated_executor_iceberg_hidden_remaining_raw(
      FloxSimulatedExecutorHandle executor, uint64_t id);
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_cancel_order(FloxSimulatedExecutorHandle executor, uint64_t order_id);
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_cancel_all(FloxSimulatedExecutorHandle executor, uint32_t symbol);
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_on_bar(FloxSimulatedExecutorHandle executor, uint32_t symbol, double close_price);
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_on_trade(FloxSimulatedExecutorHandle executor, uint32_t symbol, double price,
                                        uint8_t is_buy);
  FLOX_EXPORT(group = "simulated_executor")
  void flox_simulated_executor_advance_clock(FloxSimulatedExecutorHandle executor, int64_t timestamp_ns);
  FLOX_EXPORT(group = "simulated_executor")
  uint32_t flox_simulated_executor_fill_count(FloxSimulatedExecutorHandle executor);

  // ============================================================
  // Bar aggregation
  // ============================================================

  typedef struct
  {
    int64_t start_time_ns;
    int64_t end_time_ns;
    int64_t open_raw;
    int64_t high_raw;
    int64_t low_raw;
    int64_t close_raw;
    int64_t volume_raw;
    int64_t buy_volume_raw;
    uint32_t trade_count;
  } FloxBar;

  FLOX_EXPORT(group = "bar_aggregation")
  uint32_t flox_aggregate_time_bars(const int64_t* timestamps, const double* prices,
                                    const double* quantities, const uint8_t* is_buy, size_t len,
                                    double interval_seconds, FloxBar* bars_out,
                                    uint32_t max_bars);
  FLOX_EXPORT(group = "bar_aggregation")
  uint32_t flox_aggregate_tick_bars(const int64_t* timestamps, const double* prices,
                                    const double* quantities, const uint8_t* is_buy, size_t len,
                                    uint32_t tick_count, FloxBar* bars_out, uint32_t max_bars);
  FLOX_EXPORT(group = "bar_aggregation")
  uint32_t flox_aggregate_volume_bars(const int64_t* timestamps, const double* prices,
                                      const double* quantities, const uint8_t* is_buy, size_t len,
                                      double volume_threshold, FloxBar* bars_out,
                                      uint32_t max_bars);

  // ============================================================
  // Multi-timeframe alignment helpers
  // ============================================================
  //
  // Read the per-(symbol, timeframe) bar ring the engine populates as
  // it dispatches BarEvents to the strategy. `bar_type` matches
  // FloxBarData (0=Time, 1=Tick, ...); `param` is interval-ns for
  // time bars, count for tick bars, threshold for volume bars.

  FLOX_EXPORT(group = "multi_tf_helpers")
  uint8_t flox_strategy_last_closed_bar(FloxStrategyHandle s, uint32_t symbol,
                                        uint8_t bar_type, uint64_t param,
                                        FloxBar* out);
  FLOX_EXPORT(group = "multi_tf_helpers")
  uint32_t flox_strategy_last_n_closed_bars(FloxStrategyHandle s, uint32_t symbol,
                                            uint8_t bar_type, uint64_t param,
                                            FloxBar* bars_out, uint32_t max_bars);
  FLOX_EXPORT(group = "multi_tf_helpers")
  uint32_t flox_strategy_get_bar_ring_capacity(FloxStrategyHandle s);
  FLOX_EXPORT(group = "multi_tf_helpers")
  void flox_strategy_set_bar_ring_capacity(FloxStrategyHandle s, uint32_t capacity);

  // ============================================================
  // Multi-leg order group
  // ============================================================
  //
  // Passive state machine: legs + policy + recorded events go in,
  // aggregate state + recommended actions come out. The group does
  // no I/O — the caller wires the actions into the executor.

  typedef void* FloxOrderGroupHandle;

  // policy: 0 = BestEffort, 1 = AllOrNothing, 2 = OneSided.
  FLOX_EXPORT(group = "order_group")
  FloxOrderGroupHandle flox_order_group_create(uint64_t parent_signal_id, uint8_t policy);
  FLOX_EXPORT(group = "order_group")
  void flox_order_group_destroy(FloxOrderGroupHandle h);

  // side: 0 = BUY, 1 = SELL. Returns leg index.
  FLOX_EXPORT(group = "order_group")
  uint32_t flox_order_group_add_market_leg(FloxOrderGroupHandle h, uint32_t symbol,
                                           uint8_t side, int64_t qty_raw);
  FLOX_EXPORT(group = "order_group")
  uint32_t flox_order_group_add_limit_leg(FloxOrderGroupHandle h, uint32_t symbol,
                                          uint8_t side, int64_t price_raw,
                                          int64_t qty_raw);

  FLOX_EXPORT(group = "order_group")
  uint32_t flox_order_group_leg_count(FloxOrderGroupHandle h);
  FLOX_EXPORT(group = "order_group")
  uint8_t flox_order_group_leg_state(FloxOrderGroupHandle h, uint32_t leg_index);
  FLOX_EXPORT(group = "order_group")
  int64_t flox_order_group_leg_filled_raw(FloxOrderGroupHandle h, uint32_t leg_index);
  FLOX_EXPORT(group = "order_group")
  uint64_t flox_order_group_leg_order_id(FloxOrderGroupHandle h, uint32_t leg_index);

  FLOX_EXPORT(group = "order_group")
  void flox_order_group_record_submit(FloxOrderGroupHandle h, uint32_t leg_index,
                                      uint64_t order_id);
  FLOX_EXPORT(group = "order_group")
  void flox_order_group_record_fill(FloxOrderGroupHandle h, uint32_t leg_index,
                                    int64_t cumulative_qty_raw);
  FLOX_EXPORT(group = "order_group")
  void flox_order_group_record_cancel(FloxOrderGroupHandle h, uint32_t leg_index);
  FLOX_EXPORT(group = "order_group")
  void flox_order_group_record_failure(FloxOrderGroupHandle h, uint32_t leg_index);
  FLOX_EXPORT(group = "order_group")
  void flox_order_group_record_replace_accepted(FloxOrderGroupHandle h, uint32_t leg_index,
                                                uint64_t new_order_id);
  FLOX_EXPORT(group = "order_group")
  void flox_order_group_record_replace_rejected(FloxOrderGroupHandle h, uint32_t leg_index);

  // Returns leg index for the given exchange order id, or UINT32_MAX
  // if no leg owns this id. Used to route a replace/fill event from
  // an executor back to the correct OrderGroup leg.
  FLOX_EXPORT(group = "order_group")
  uint32_t flox_order_group_find_leg_by_order_id(FloxOrderGroupHandle h, uint64_t order_id);

  FLOX_EXPORT(group = "order_group")
  uint8_t flox_order_group_state(FloxOrderGroupHandle h);

  // Recommended-actions output. Each action is laid out as 5 i64 slots:
  //   [0] kind (0=CancelLeg, 1=RevertLeg)
  //   [1] leg_index
  //   [2] order_id (CancelLeg) | symbol_id (RevertLeg)
  //   [3] side (RevertLeg, 0=BUY/1=SELL) | 0
  //   [4] qty_raw (RevertLeg) | 0
  // Returns number of actions written; bounded by max_actions.
  FLOX_EXPORT(group = "order_group")
  uint32_t flox_order_group_recommended_actions(FloxOrderGroupHandle h,
                                                int64_t* actions_out,
                                                uint32_t max_actions);

  // Mark a leg's action as dispatched so it stops surfacing in
  // `recommended_actions`. Called by the auto-dispatch helper after
  // each emit. kind: 0 = CancelLeg, 1 = RevertLeg.
  FLOX_EXPORT(group = "order_group")
  void flox_order_group_mark_action_dispatched(FloxOrderGroupHandle h, uint32_t leg_index,
                                               uint8_t kind);

  // Group-level risk limits. Zero in any field means "no cap on
  // that dimension". Pass marketRefPrices_raw with one entry per
  // leg in addMarketLeg order; entries for limit legs are ignored.
  FLOX_EXPORT(group = "order_group")
  void flox_order_group_set_risk_limits(FloxOrderGroupHandle h,
                                        int64_t max_gross_notional_raw,
                                        double max_concentration_pct,
                                        int64_t max_leg_qty_raw);

  // Run the configured limits against the current legs. Returns 1 if
  // a breach was found, 0 otherwise. When 1, the breach rule + detail
  // are written into the caller-supplied buffers (truncated at the
  // given capacities). `equity` only matters when
  // max_concentration_pct > 0.
  FLOX_EXPORT(group = "order_group")
  uint8_t flox_order_group_precheck_submission(FloxOrderGroupHandle h, double equity,
                                               const int64_t* market_ref_prices_raw,
                                               uint32_t market_ref_prices_len,
                                               char* rule_out, size_t rule_capacity,
                                               char* detail_out, size_t detail_capacity);

  // OneSided pair latency budget. After the leader submits, the
  // strategy waits up to `budget_ns` of feed-time for the leader's
  // ack before sending the follower. Zero disables the gate.
  FLOX_EXPORT(group = "order_group")
  void flox_order_group_set_pair_latency_budget_ns(FloxOrderGroupHandle h,
                                                   int64_t budget_ns);

  // Returns the decision the strategy should act on:
  //   0 = Wait, 1 = SubmitFollower, 2 = CancelLeader.
  // Pass `ack_received = 1` and the actual ack ts when the leader has
  // acked; pass 0 + the current feed-time as `leader_ack_ts_ns` to
  // poll for timeout.
  FLOX_EXPORT(group = "order_group")
  uint8_t flox_order_group_pair_latency_decision(FloxOrderGroupHandle h,
                                                 int64_t leader_submit_ts_ns,
                                                 int64_t leader_ack_ts_ns,
                                                 uint8_t ack_received);

  // ============================================================
  // Multi-feed clock
  // ============================================================
  //
  // Latency-aware multi-feed wait policy. The strategy creates a
  // clock for a list of symbols + a policy + a staleness budget; on
  // each tick the clock decides whether to fire and reports per-feed
  // staleness so the strategy can weight or skip a decision.
  //
  // The snapshot is read through accessor functions (no struct
  // returned by value over the C ABI) so bindings can shape it
  // however they want.

  typedef void* FloxFeedClockHandle;

  // policy: 0 = WaitForAll, 1 = FireOnAny, 2 = LeaderFollower.
  // leader_symbol: ignored unless policy == LeaderFollower; defaults
  // to symbols[0] when zero.
  FLOX_EXPORT(group = "feed_clock")
  FloxFeedClockHandle flox_feed_clock_create(const uint32_t* symbols, uint32_t symbol_count,
                                             uint8_t policy, int64_t timeout_ms,
                                             uint32_t leader_symbol,
                                             int64_t staleness_budget_ms);
  FLOX_EXPORT(group = "feed_clock")
  void flox_feed_clock_destroy(FloxFeedClockHandle h);

  FLOX_EXPORT(group = "feed_clock")
  uint32_t flox_feed_clock_symbol_count(FloxFeedClockHandle h);
  FLOX_EXPORT(group = "feed_clock")
  uint32_t flox_feed_clock_symbol_at(FloxFeedClockHandle h, uint32_t index);

  // Drive a tick. Returns 1 if the clock fired, 0 otherwise. The
  // snapshot it produced (last-ts and staleness per registered feed)
  // is cached on the clock and read with the accessors below.
  FLOX_EXPORT(group = "feed_clock")
  uint8_t flox_feed_clock_tick(FloxFeedClockHandle h, int64_t ts_ns, uint32_t symbol);

  // Last computed snapshot. `triggered_by` is the symbol that
  // produced the most recent tick.
  FLOX_EXPORT(group = "feed_clock")
  uint8_t flox_feed_clock_last_fired(FloxFeedClockHandle h);
  FLOX_EXPORT(group = "feed_clock")
  uint32_t flox_feed_clock_last_triggered_by(FloxFeedClockHandle h);
  FLOX_EXPORT(group = "feed_clock")
  int64_t flox_feed_clock_last_seen_at(FloxFeedClockHandle h, uint32_t index);
  FLOX_EXPORT(group = "feed_clock")
  int64_t flox_feed_clock_staleness_at(FloxFeedClockHandle h, uint32_t index);

  FLOX_EXPORT(group = "feed_clock")
  void flox_feed_clock_reset(FloxFeedClockHandle h);

  // ============================================================
  // Position tracking
  // ============================================================

  FLOX_EXPORT(group = "position")
  FloxPositionTrackerHandle flox_position_tracker_create(uint8_t cost_basis);
  FLOX_EXPORT(group = "position")
  void flox_position_tracker_destroy(FloxPositionTrackerHandle tracker);
  FLOX_EXPORT(group = "position")
  void flox_position_tracker_on_fill(FloxPositionTrackerHandle tracker, uint32_t symbol,
                                     uint8_t side, double price, double quantity);
  FLOX_EXPORT(group = "position")
  double flox_position_tracker_position(FloxPositionTrackerHandle tracker, uint32_t symbol);
  FLOX_EXPORT(group = "position")
  double flox_position_tracker_avg_entry(FloxPositionTrackerHandle tracker, uint32_t symbol);
  FLOX_EXPORT(group = "position")
  double flox_position_tracker_realized_pnl(FloxPositionTrackerHandle tracker, uint32_t symbol);
  FLOX_EXPORT(group = "position")
  double flox_position_tracker_total_pnl(FloxPositionTrackerHandle tracker);

  // ============================================================
  // Volume profile
  // ============================================================

  FLOX_EXPORT(group = "volume_profile")
  FloxVolumeProfileHandle flox_volume_profile_create(double tick_size);
  FLOX_EXPORT(group = "volume_profile")
  void flox_volume_profile_destroy(FloxVolumeProfileHandle profile);
  FLOX_EXPORT(group = "volume_profile")
  void flox_volume_profile_add_trade(FloxVolumeProfileHandle profile, double price, double quantity,
                                     uint8_t is_buy);
  FLOX_EXPORT(group = "volume_profile")
  double flox_volume_profile_poc(FloxVolumeProfileHandle profile);
  FLOX_EXPORT(group = "volume_profile")
  double flox_volume_profile_vah(FloxVolumeProfileHandle profile);
  FLOX_EXPORT(group = "volume_profile")
  double flox_volume_profile_val(FloxVolumeProfileHandle profile);
  FLOX_EXPORT(group = "volume_profile")
  double flox_volume_profile_total_volume(FloxVolumeProfileHandle profile);
  FLOX_EXPORT(group = "volume_profile")
  double flox_volume_profile_total_delta(FloxVolumeProfileHandle profile);
  FLOX_EXPORT(group = "volume_profile")
  uint32_t flox_volume_profile_num_levels(FloxVolumeProfileHandle profile);
  FLOX_EXPORT(group = "volume_profile")
  void flox_volume_profile_clear(FloxVolumeProfileHandle profile);

  // ============================================================
  // Footprint bar
  // ============================================================

  FLOX_EXPORT(group = "footprint_bar")
  FloxFootprintHandle flox_footprint_create(double tick_size);
  FLOX_EXPORT(group = "footprint_bar")
  void flox_footprint_destroy(FloxFootprintHandle footprint);
  FLOX_EXPORT(group = "footprint_bar")
  void flox_footprint_add_trade(FloxFootprintHandle footprint, double price, double quantity,
                                uint8_t is_buy);
  FLOX_EXPORT(group = "footprint_bar")
  double flox_footprint_total_delta(FloxFootprintHandle footprint);
  FLOX_EXPORT(group = "footprint_bar")
  double flox_footprint_total_volume(FloxFootprintHandle footprint);
  FLOX_EXPORT(group = "footprint_bar")
  uint32_t flox_footprint_num_levels(FloxFootprintHandle footprint);
  FLOX_EXPORT(group = "footprint_bar")
  void flox_footprint_clear(FloxFootprintHandle footprint);

  // ============================================================
  // Statistics
  // ============================================================

  FLOX_EXPORT(group = "statistics")
  double flox_stat_correlation(const double* x, const double* y, size_t len);
  FLOX_EXPORT(group = "statistics")
  double flox_stat_profit_factor(const double* pnl, size_t len);
  FLOX_EXPORT(group = "statistics")
  double flox_stat_win_rate(const double* pnl, size_t len);

  // ============================================================
  // Order journey tracer
  // ============================================================

  // One row of the order-journey trace. Mirrors flox::OrderTraceRecord.
  typedef struct
  {
    uint64_t order_id;
    uint32_t seq;
    uint8_t status;
    uint8_t is_maker;
    uint8_t _pad[2];
    int64_t ts_ns;
    int64_t fill_qty_raw;
    int64_t fill_price_raw;
    int64_t queue_ahead_raw;
    int64_t queue_total_raw;
    int64_t submitted_at_ns;
    int64_t accepted_at_ns;
    int64_t first_fill_at_ns;
    int64_t last_fill_at_ns;
    int64_t canceled_at_ns;
    int64_t rejected_at_ns;
    int64_t triggered_at_ns;
    int64_t expired_at_ns;
  } FloxOrderTraceRow;

  FLOX_EXPORT(group = "order_journey_tracer")
  FloxOrderJourneyTracerHandle flox_order_journey_tracer_create(
      uint64_t max_orders, uint64_t max_records_per_order, double sample_rate,
      uint64_t sample_salt);
  FLOX_EXPORT(group = "order_journey_tracer")
  void flox_order_journey_tracer_destroy(FloxOrderJourneyTracerHandle tracer);
  FLOX_EXPORT(group = "order_journey_tracer")
  uint64_t flox_order_journey_tracer_order_count(FloxOrderJourneyTracerHandle tracer);
  FLOX_EXPORT(group = "order_journey_tracer")
  uint64_t flox_order_journey_tracer_record_count(FloxOrderJourneyTracerHandle tracer);
  FLOX_EXPORT(group = "order_journey_tracer")
  double flox_order_journey_tracer_median_ack_latency_ns(
      FloxOrderJourneyTracerHandle tracer);
  FLOX_EXPORT(group = "order_journey_tracer")
  double flox_order_journey_tracer_median_time_to_first_fill_ns(
      FloxOrderJourneyTracerHandle tracer);
  FLOX_EXPORT(group = "order_journey_tracer")
  double flox_order_journey_tracer_maker_fill_ratio(
      FloxOrderJourneyTracerHandle tracer);
  FLOX_EXPORT(group = "order_journey_tracer")
  double flox_order_journey_tracer_cancel_race_loss_rate(
      FloxOrderJourneyTracerHandle tracer);
  // Fills out (up to max_rows) with the full trace; returns the actual row
  // count written. Pass max_rows = 0 to query the required size only.
  FLOX_EXPORT(group = "order_journey_tracer")
  uint64_t flox_order_journey_tracer_result(FloxOrderJourneyTracerHandle tracer,
                                            FloxOrderTraceRow* out,
                                            uint64_t max_rows);
  // Same shape for a single order's trace.
  FLOX_EXPORT(group = "order_journey_tracer")
  uint64_t flox_order_journey_tracer_journey(FloxOrderJourneyTracerHandle tracer,
                                             uint64_t order_id,
                                             FloxOrderTraceRow* out,
                                             uint64_t max_rows);
  FLOX_EXPORT(group = "order_journey_tracer")
  void flox_order_journey_tracer_clear(FloxOrderJourneyTracerHandle tracer);
  // Attach a journey tracer to a BacktestRunner. The runner takes the
  // handle by void* to keep this declaration position-independent; see
  // flox_backtest_runner_add_journey_tracer in the backtest section.

  // ============================================================
  // Order tracker
  // ============================================================

  typedef void* FloxOrderTrackerHandle;

  FLOX_EXPORT(group = "order_tracker")
  FloxOrderTrackerHandle flox_order_tracker_create(void);
  FLOX_EXPORT(group = "order_tracker")
  void flox_order_tracker_destroy(FloxOrderTrackerHandle tracker);
  FLOX_EXPORT(group = "order_tracker")
  uint8_t flox_order_tracker_on_submitted(FloxOrderTrackerHandle tracker, uint64_t order_id,
                                          uint32_t symbol, uint8_t side, double price, double qty);
  FLOX_EXPORT(group = "order_tracker")
  uint8_t flox_order_tracker_on_filled(FloxOrderTrackerHandle tracker, uint64_t order_id,
                                       double fill_qty);
  FLOX_EXPORT(group = "order_tracker")
  uint8_t flox_order_tracker_on_canceled(FloxOrderTrackerHandle tracker, uint64_t order_id);
  FLOX_EXPORT(group = "order_tracker")
  uint8_t flox_order_tracker_is_active(FloxOrderTrackerHandle tracker, uint64_t order_id);
  FLOX_EXPORT(group = "order_tracker")
  uint32_t flox_order_tracker_active_count(FloxOrderTrackerHandle tracker);
  FLOX_EXPORT(group = "order_tracker")
  uint32_t flox_order_tracker_total_count(FloxOrderTrackerHandle tracker);
  FLOX_EXPORT(group = "order_tracker")
  void flox_order_tracker_prune(FloxOrderTrackerHandle tracker);

  // ============================================================
  // Position group tracker
  // ============================================================

  typedef void* FloxPositionGroupHandle;

  FLOX_EXPORT(group = "position_group")
  FloxPositionGroupHandle flox_position_group_create(void);
  FLOX_EXPORT(group = "position_group")
  void flox_position_group_destroy(FloxPositionGroupHandle tracker);
  FLOX_EXPORT(group = "position_group")
  uint64_t flox_position_group_open(FloxPositionGroupHandle tracker, uint64_t order_id,
                                    uint32_t symbol, uint8_t side, double price, double qty);
  FLOX_EXPORT(group = "position_group")
  void flox_position_group_close(FloxPositionGroupHandle tracker, uint64_t position_id,
                                 double exit_price);
  FLOX_EXPORT(group = "position_group")
  void flox_position_group_partial_close(FloxPositionGroupHandle tracker, uint64_t position_id,
                                         double qty, double exit_price);
  FLOX_EXPORT(group = "position_group")
  double flox_position_group_net_position(FloxPositionGroupHandle tracker, uint32_t symbol);
  FLOX_EXPORT(group = "position_group")
  double flox_position_group_realized_pnl(FloxPositionGroupHandle tracker, uint32_t symbol);
  FLOX_EXPORT(group = "position_group")
  double flox_position_group_total_pnl(FloxPositionGroupHandle tracker);
  FLOX_EXPORT(group = "position_group")
  uint32_t flox_position_group_open_count(FloxPositionGroupHandle tracker, uint32_t symbol);
  FLOX_EXPORT(group = "position_group")
  void flox_position_group_prune(FloxPositionGroupHandle tracker);

  // ============================================================
  // Market profile
  // ============================================================

  typedef void* FloxMarketProfileHandle;

  FLOX_EXPORT(group = "market_profile")
  FloxMarketProfileHandle flox_market_profile_create(double tick_size, uint32_t period_minutes,
                                                     int64_t session_start_ns);
  FLOX_EXPORT(group = "market_profile")
  void flox_market_profile_destroy(FloxMarketProfileHandle profile);
  FLOX_EXPORT(group = "market_profile")
  void flox_market_profile_add_trade(FloxMarketProfileHandle profile, int64_t timestamp_ns,
                                     double price, double qty, uint8_t is_buy);
  FLOX_EXPORT(group = "market_profile")
  double flox_market_profile_poc(FloxMarketProfileHandle profile);
  FLOX_EXPORT(group = "market_profile")
  double flox_market_profile_vah(FloxMarketProfileHandle profile);
  FLOX_EXPORT(group = "market_profile")
  double flox_market_profile_val(FloxMarketProfileHandle profile);
  FLOX_EXPORT(group = "market_profile")
  double flox_market_profile_ib_high(FloxMarketProfileHandle profile);
  FLOX_EXPORT(group = "market_profile")
  double flox_market_profile_ib_low(FloxMarketProfileHandle profile);
  FLOX_EXPORT(group = "market_profile")
  uint8_t flox_market_profile_is_poor_high(FloxMarketProfileHandle profile);
  FLOX_EXPORT(group = "market_profile")
  uint8_t flox_market_profile_is_poor_low(FloxMarketProfileHandle profile);
  FLOX_EXPORT(group = "market_profile")
  uint32_t flox_market_profile_num_levels(FloxMarketProfileHandle profile);
  FLOX_EXPORT(group = "market_profile")
  void flox_market_profile_clear(FloxMarketProfileHandle profile);

  // ============================================================
  // Composite book matrix
  // ============================================================

  typedef void* FloxCompositeBookHandle;

  FLOX_EXPORT(group = "composite_book")
  FloxCompositeBookHandle flox_composite_book_create(void);
  FLOX_EXPORT(group = "composite_book")
  void flox_composite_book_destroy(FloxCompositeBookHandle book);
  FLOX_EXPORT(group = "composite_book")
  uint8_t flox_composite_book_best_bid(FloxCompositeBookHandle book, uint32_t symbol,
                                       double* price_out, double* qty_out);
  FLOX_EXPORT(group = "composite_book")
  uint8_t flox_composite_book_best_ask(FloxCompositeBookHandle book, uint32_t symbol,
                                       double* price_out, double* qty_out);
  FLOX_EXPORT(group = "composite_book")
  uint8_t flox_composite_book_has_arb(FloxCompositeBookHandle book, uint32_t symbol);
  FLOX_EXPORT(group = "composite_book")
  void flox_composite_book_mark_stale(FloxCompositeBookHandle book, uint32_t exchange,
                                      uint32_t symbol);
  FLOX_EXPORT(group = "composite_book")
  void flox_composite_book_check_staleness(FloxCompositeBookHandle book, int64_t now_ns,
                                           int64_t threshold_ns);

  // ============================================================
  // Executor fill access
  // ============================================================

  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol;
    uint8_t side;
    int64_t price_raw;
    int64_t quantity_raw;
    int64_t timestamp_ns;
  } FloxFill;

  FLOX_EXPORT(group = "executor_fill")
  uint32_t flox_simulated_executor_get_fills(FloxSimulatedExecutorHandle executor, FloxFill* fills_out,
                                             uint32_t max_fills);

  // ============================================================
  // Additional bar aggregation
  // ============================================================

  FLOX_EXPORT(group = "additional_bar")
  uint32_t flox_aggregate_range_bars(const int64_t* timestamps, const double* prices,
                                     const double* quantities, const uint8_t* is_buy, size_t len,
                                     double range_size, FloxBar* bars_out, uint32_t max_bars);
  FLOX_EXPORT(group = "additional_bar")
  uint32_t flox_aggregate_renko_bars(const int64_t* timestamps, const double* prices,
                                     const double* quantities, const uint8_t* is_buy, size_t len,
                                     double brick_size, FloxBar* bars_out, uint32_t max_bars);

  // ============================================================
  // L3 Order book
  // ============================================================

  typedef void* FloxL3BookHandle;

  FLOX_EXPORT(group = "l3_order")
  FloxL3BookHandle flox_l3_book_create(void);
  FLOX_EXPORT(group = "l3_order")
  void flox_l3_book_destroy(FloxL3BookHandle book);
  FLOX_EXPORT(group = "l3_order")
  int32_t flox_l3_book_add_order(FloxL3BookHandle book, uint64_t order_id, double price,
                                 double quantity, uint8_t side);
  FLOX_EXPORT(group = "l3_order")
  int32_t flox_l3_book_remove_order(FloxL3BookHandle book, uint64_t order_id);
  FLOX_EXPORT(group = "l3_order")
  int32_t flox_l3_book_modify_order(FloxL3BookHandle book, uint64_t order_id, double new_qty);
  FLOX_EXPORT(group = "l3_order")
  uint8_t flox_l3_book_best_bid(FloxL3BookHandle book, double* price_out);
  FLOX_EXPORT(group = "l3_order")
  uint8_t flox_l3_book_best_ask(FloxL3BookHandle book, double* price_out);
  FLOX_EXPORT(group = "l3_order")
  double flox_l3_book_bid_at_price(FloxL3BookHandle book, double price);
  FLOX_EXPORT(group = "l3_order")
  double flox_l3_book_ask_at_price(FloxL3BookHandle book, double price);

  // ============================================================
  // Data writer (binary log)
  // ============================================================

  typedef void* FloxDataWriterHandle;

  FLOX_EXPORT(group = "data_writer")
  FloxDataWriterHandle flox_data_writer_create(const char* output_dir, uint64_t max_segment_mb,
                                               uint8_t exchange_id);
  FLOX_EXPORT(group = "data_writer")
  void flox_data_writer_destroy(FloxDataWriterHandle writer);
  FLOX_EXPORT(group = "data_writer")
  uint8_t flox_data_writer_write_trade(FloxDataWriterHandle writer, int64_t exchange_ts_ns,
                                       int64_t recv_ts_ns, double price, double qty,
                                       uint64_t trade_id, uint32_t symbol_id, uint8_t side);
  // Single book-update writer. Levels are raw int64 (Price/Quantity
  // scale = 1e8); no double conversion in the hot path. Returns 1 on
  // success, 0 on failure (e.g. writer closed). bids/asks may be NULL
  // when the matching count is 0.
  FLOX_EXPORT(group = "data_writer")
  uint8_t flox_data_writer_write_book(FloxDataWriterHandle writer,
                                      int64_t exchange_ts_ns,
                                      int64_t recv_ts_ns,
                                      int64_t seq,
                                      uint32_t symbol_id,
                                      uint8_t is_snapshot,
                                      const FloxBookLevel* bids, uint32_t n_bids,
                                      const FloxBookLevel* asks, uint32_t n_asks);
  FLOX_EXPORT(group = "data_writer")
  void flox_data_writer_flush(FloxDataWriterHandle writer);
  FLOX_EXPORT(group = "data_writer")
  void flox_data_writer_close(FloxDataWriterHandle writer);

  // ============================================================
  // Data reader (binary log)
  // ============================================================

  typedef void* FloxDataReaderHandle;

  FLOX_EXPORT(group = "data_reader")
  FloxDataReaderHandle flox_data_reader_create(const char* data_dir);
  FLOX_EXPORT(group = "data_reader")
  void flox_data_reader_destroy(FloxDataReaderHandle reader);
  FLOX_EXPORT(group = "data_reader")
  uint64_t flox_data_reader_count(FloxDataReaderHandle reader);

  // ============================================================
  // Order book level access
  // ============================================================

  FLOX_EXPORT(group = "order_book")
  uint32_t flox_book_get_bids(FloxBookHandle book, double* prices_out, double* qtys_out,
                              uint32_t max_levels);
  FLOX_EXPORT(group = "order_book")
  uint32_t flox_book_get_asks(FloxBookHandle book, double* prices_out, double* qtys_out,
                              uint32_t max_levels);

  // ============================================================
  // Heikin-Ashi bar aggregation
  // ============================================================

  FLOX_EXPORT(group = "heikin_ashi")
  uint32_t flox_aggregate_heikin_ashi_bars(const int64_t* timestamps, const double* prices,
                                           const double* quantities, const uint8_t* is_buy,
                                           size_t len, double interval_seconds, FloxBar* bars_out,
                                           uint32_t max_bars);

  // ============================================================
  // Additional stats
  // ============================================================

  FLOX_EXPORT(group = "additional_stats")
  double flox_stat_permutation_test(const double* group1, size_t len1, const double* group2,
                                    size_t len2, uint32_t num_permutations);
  FLOX_EXPORT(group = "additional_stats")
  void flox_stat_bootstrap_ci(const double* data, size_t len, double confidence,
                              uint32_t num_samples, double* lower_out, double* median_out,
                              double* upper_out);
  FLOX_EXPORT(group = "additional_stats")
  void flox_stat_whites_reality_check(const double* returns, size_t num_strategies,
                                      size_t num_periods, uint32_t num_bootstrap,
                                      double avg_block_size, double* p_value_out,
                                      double* best_stat_out, int32_t* best_index_out);

  // ============================================================
  // Segment operations
  // ============================================================

  FLOX_EXPORT(group = "segment")
  uint8_t flox_segment_validate(const char* path);
  FLOX_EXPORT(group = "segment")
  uint8_t flox_segment_merge(const char* input_dir, const char* output_path);

  // ============================================================
  // Backtest: slippage, queue, result, metrics, equity curve
  // ============================================================

  typedef enum
  {
    FLOX_SLIPPAGE_NONE = 0,
    FLOX_SLIPPAGE_FIXED_TICKS = 1,
    FLOX_SLIPPAGE_FIXED_BPS = 2,
    FLOX_SLIPPAGE_VOLUME_IMPACT = 3
  } FloxSlippageModel;

  typedef enum
  {
    FLOX_QUEUE_NONE = 0,
    FLOX_QUEUE_TOB = 1,
    FLOX_QUEUE_FULL = 2,
    FLOX_QUEUE_PRO_RATA = 3,
    FLOX_QUEUE_PRO_RATA_WITH_FIFO = 4
  } FloxQueueModel;

  // Configure slippage. Applies to market-style fills on all symbols unless
  // a per-symbol override is set. `tick_size` is the venue tick size in
  // price units (e.g. 0.01 for 1-cent ticks); pass 0.0 to fall back to one
  // raw price unit.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_default_slippage(FloxSimulatedExecutorHandle executor,
                                                    int32_t model, int32_t ticks,
                                                    double tick_size, double bps,
                                                    double impact_coeff);
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_symbol_slippage(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                                   int32_t model, int32_t ticks,
                                                   double tick_size, double bps,
                                                   double impact_coeff);

  // Configure queue simulation for limit orders.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_queue_model(FloxSimulatedExecutorHandle executor, int32_t model,
                                               uint32_t depth);

  // For FLOX_QUEUE_PRO_RATA_WITH_FIFO: the first N orders at a level
  // consume the trade FIFO; the remainder is split pro-rata across
  // the rest. Ignored in NONE / TOB / FULL / PRO_RATA modes.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_queue_fifo_top_n(FloxSimulatedExecutorHandle executor,
                                                    uint32_t top_n);

  // TOP_PRO_LMM: fraction of each trade reserved for the queue-front
  // order; default 0.40. Ignored in other queue models.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_top_priority_share(FloxSimulatedExecutorHandle executor,
                                                      double share);

  // TOP_PRO_LMM: mark these order ids as Lead Market Makers.
  // `ids` is an array of `n_ids` order ids.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_lmm_orders(FloxSimulatedExecutorHandle executor,
                                              const uint64_t* ids, uint32_t n_ids);

  // TOP_PRO_LMM: LMM bonus multiplier applied during the tail
  // pro-rata distribution. Default 1.5.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_lmm_bonus_multiplier(
      FloxSimulatedExecutorHandle executor, double multiplier);

  // TOP_PRO_LMM / PRO_RATA_WITH_PRIORITY: per-order priority weight.
  // Defaults to 1.0 when unset.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_order_priority_multiplier(
      FloxSimulatedExecutorHandle executor, uint64_t order_id, double multiplier);

  // Set submit / cancel / replace ack latency knobs directly (without
  // touching queue model or slippage). Jitter is the uniform band on
  // top of the base latency; both are in nanoseconds.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_submit_ack_latency(FloxSimulatedExecutorHandle executor,
                                                      int64_t latency_ns, int64_t jitter_ns);
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_cancel_ack_latency(FloxSimulatedExecutorHandle executor,
                                                      int64_t latency_ns, int64_t jitter_ns);
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_replace_ack_latency(FloxSimulatedExecutorHandle executor,
                                                       int64_t latency_ns, int64_t jitter_ns);

  // Apply a named latency profile. Valid names: binance_um_futures,
  // bybit_linear, okx_swap, deribit, idealized, adversarial. Unknown
  // names are a no-op.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_apply_latency_profile(FloxSimulatedExecutorHandle executor,
                                                     const char* profile_name);

  // Self-trade prevention mode. 0=None, 1=CancelNewest, 2=CancelOldest,
  // 3=CancelBoth, 4=Decrement. Default None.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_stp_mode(FloxSimulatedExecutorHandle executor,
                                            uint8_t mode);

  // FOK fill semantics. 0=any_price (default; multi-level walk), 1=single_price
  // (only the limit-price level counts). See FokMode in
  // flox/backtest/simulated_executor.h for details.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_fok_mode(FloxSimulatedExecutorHandle executor,
                                            uint8_t mode);
  FLOX_EXPORT(group = "backtest_slippage")
  uint8_t flox_simulated_executor_fok_mode(FloxSimulatedExecutorHandle executor);

  // Multi-account STP group membership. Two orders share an STP scope
  // when their accountIds match OR both accounts map into the same
  // non-zero group. group_id=0 removes any prior mapping.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_stp_group_membership(
      FloxSimulatedExecutorHandle executor, uint64_t account_id, uint64_t group_id);
  FLOX_EXPORT(group = "backtest_slippage")
  uint64_t flox_simulated_executor_stp_group_for(FloxSimulatedExecutorHandle executor,
                                                 uint64_t account_id);

  // Latency-distribution handle. The simulator copies the distribution
  // on set_*_distribution; the caller still owns and destroys the
  // handle. kind: 0=Constant, 1=Uniform, 2=Lognormal, 3=Empirical.
  typedef void* FloxLatencyDistributionHandle;

  FLOX_EXPORT(group = "backtest_slippage")
  FloxLatencyDistributionHandle flox_latency_distribution_create(void);
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_latency_distribution_destroy(FloxLatencyDistributionHandle h);

  // Mutate the held distribution to one of the four shapes. Subsequent
  // calls overwrite. Each leaves burst-correlation at its current
  // value.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_latency_distribution_set_constant(FloxLatencyDistributionHandle h, int64_t ns);
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_latency_distribution_set_uniform(FloxLatencyDistributionHandle h, int64_t lo_ns,
                                             int64_t hi_ns);
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_latency_distribution_set_lognormal(FloxLatencyDistributionHandle h,
                                               int64_t median_ns, double sigma);
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_latency_distribution_set_empirical(FloxLatencyDistributionHandle h,
                                               const int64_t* samples_ns, uint32_t n_samples);

  // Burst correlation in [0, 1). Higher = stronger autocorrelation
  // between consecutive draws (AR(1) on log-residuals for Lognormal).
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_latency_distribution_set_burst_correlation(FloxLatencyDistributionHandle h,
                                                       double rho);

  FLOX_EXPORT(group = "backtest_slippage")
  int64_t flox_latency_distribution_median_ns(FloxLatencyDistributionHandle h);

  // Distribution-based ack-latency setters on SimulatedExecutor.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_submit_ack_latency_distribution(
      FloxSimulatedExecutorHandle executor, FloxLatencyDistributionHandle dist);
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_cancel_ack_latency_distribution(
      FloxSimulatedExecutorHandle executor, FloxLatencyDistributionHandle dist);
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_set_replace_ack_latency_distribution(
      FloxSimulatedExecutorHandle executor, FloxLatencyDistributionHandle dist);

  // Client-side rate-limit policy. Simulator copies the policy on
  // set_rate_limit_policy; caller still owns + destroys the handle.
  typedef void* FloxRateLimitPolicyHandle;

  FLOX_EXPORT(group = "rate_limit")
  FloxRateLimitPolicyHandle flox_rate_limit_policy_create(void);
  FLOX_EXPORT(group = "rate_limit")
  void flox_rate_limit_policy_destroy(FloxRateLimitPolicyHandle h);

  FLOX_EXPORT(group = "rate_limit")
  void flox_rate_limit_policy_add_bucket(FloxRateLimitPolicyHandle h, const char* name,
                                         int64_t window_ns, uint32_t capacity,
                                         uint32_t submit_weight, uint32_t cancel_weight,
                                         uint32_t replace_weight);

  // Add a bucket scoped to an endpoint family. `family`: 0=Trading,
  // 1=MarketData, 2=Account. `query_weight` applies when the action
  // is QueryAccount / QueryMarketData; the submit / cancel / replace
  // weights only apply when family=Trading.
  FLOX_EXPORT(group = "rate_limit")
  void flox_rate_limit_policy_add_bucket_family(FloxRateLimitPolicyHandle h, const char* name,
                                                int64_t window_ns, uint32_t capacity,
                                                uint32_t submit_weight, uint32_t cancel_weight,
                                                uint32_t replace_weight,
                                                uint8_t family, uint32_t query_weight);

  FLOX_EXPORT(group = "rate_limit")
  void flox_rate_limit_policy_set_ban(FloxRateLimitPolicyHandle h,
                                      uint32_t after_consecutive_rejects,
                                      int64_t ban_duration_ns);

  // Canned profiles: "binance_um_futures", "bybit_linear",
  // "okx_swap", "deribit". Unknown names are a no-op.
  FLOX_EXPORT(group = "rate_limit")
  void flox_rate_limit_policy_load_profile(FloxRateLimitPolicyHandle h,
                                           const char* profile_name);

  FLOX_EXPORT(group = "rate_limit")
  int64_t flox_rate_limit_policy_ban_until_ns(FloxRateLimitPolicyHandle h);
  FLOX_EXPORT(group = "rate_limit")
  uint32_t flox_rate_limit_policy_consecutive_rejects(FloxRateLimitPolicyHandle h);

  // Per-bucket readback. Two-call: pass out_buf=NULL to learn the
  // count; pass a buffer with `max_buckets * 4` int64 slots laid as
  // [window_ns, used, capacity, _pad] per bucket to read. Bucket
  // names are not surfaced via this call; use the bucket index +
  // your original add_bucket ordering.
  FLOX_EXPORT(group = "rate_limit")
  uint32_t flox_rate_limit_policy_bucket_state(FloxRateLimitPolicyHandle h, int64_t now_ns,
                                               int64_t* out_buf, uint32_t max_buckets);

  FLOX_EXPORT(group = "rate_limit")
  void flox_simulated_executor_set_rate_limit_policy(FloxSimulatedExecutorHandle executor,
                                                     FloxRateLimitPolicyHandle policy);
  FLOX_EXPORT(group = "rate_limit")
  void flox_simulated_executor_clear_rate_limit_policy(FloxSimulatedExecutorHandle executor);

  // Venue downtime model. Caller owns + destroys the handle; the
  // simulator holds a non-owning pointer that lives as long as the
  // executor uses it. set_venue_availability(executor, NULL) detaches.
  typedef void* FloxVenueAvailabilityHandle;

  FLOX_EXPORT(group = "venue_availability")
  FloxVenueAvailabilityHandle flox_venue_availability_create(void);
  FLOX_EXPORT(group = "venue_availability")
  void flox_venue_availability_destroy(FloxVenueAvailabilityHandle h);

  // Add a scheduled outage. policy: 0=CANCEL_ALL, 1=HOLD,
  // 2=EXPIRE_GTC_AFTER. gtc_ttl_ns is honored for EXPIRE_GTC_AFTER
  // only.
  FLOX_EXPORT(group = "venue_availability")
  void flox_venue_availability_schedule_outage(FloxVenueAvailabilityHandle h,
                                               int64_t start_ns, int64_t duration_ns,
                                               uint8_t policy, int64_t gtc_ttl_ns);

  // Extended schedule with outage pathology. outage_type:
  // 0=Total (legacy), 1=SubmitOnlyDown, 2=CancelOnlyDown,
  // 3=SlowDegradation, 4=StaleBook, 5=WrongSideRecovery.
  FLOX_EXPORT(group = "venue_availability")
  void flox_venue_availability_schedule_outage_ex(
      FloxVenueAvailabilityHandle h, int64_t start_ns, int64_t duration_ns,
      uint8_t outage_type, uint8_t policy, int64_t gtc_ttl_ns,
      double degradation_latency_multiplier, double wrong_side_recovery_bps);

  // Per-action / per-feed gates.
  FLOX_EXPORT(group = "venue_availability")
  uint8_t flox_venue_availability_submits_allowed(FloxVenueAvailabilityHandle h,
                                                  int64_t now_ns);
  FLOX_EXPORT(group = "venue_availability")
  uint8_t flox_venue_availability_cancels_allowed(FloxVenueAvailabilityHandle h,
                                                  int64_t now_ns);
  FLOX_EXPORT(group = "venue_availability")
  uint8_t flox_venue_availability_book_updates_allowed(FloxVenueAvailabilityHandle h,
                                                       int64_t now_ns);
  FLOX_EXPORT(group = "venue_availability")
  uint8_t flox_venue_availability_trades_allowed(FloxVenueAvailabilityHandle h,
                                                 int64_t now_ns);
  FLOX_EXPORT(group = "venue_availability")
  double flox_venue_availability_latency_multiplier(FloxVenueAvailabilityHandle h,
                                                    int64_t now_ns);
  FLOX_EXPORT(group = "venue_availability")
  double flox_venue_availability_consume_wrong_side_recovery_bps(
      FloxVenueAvailabilityHandle h);

  // Enable Poisson random outages. per_day is the expected number of
  // outages per UTC day; mean_duration_ns is the mean exponential
  // duration. seed makes sampling reproducible.
  FLOX_EXPORT(group = "venue_availability")
  void flox_venue_availability_auto_random_outages(FloxVenueAvailabilityHandle h,
                                                   double per_day, int64_t mean_duration_ns,
                                                   uint8_t policy, uint64_t seed);

  FLOX_EXPORT(group = "venue_availability")
  uint8_t flox_venue_availability_is_up(FloxVenueAvailabilityHandle h, int64_t now_ns);

  FLOX_EXPORT(group = "venue_availability")
  void flox_simulated_executor_set_venue_availability(FloxSimulatedExecutorHandle executor,
                                                      FloxVenueAvailabilityHandle availability);

  // Feed a trade with quantity (enables queue-fill simulation for limit orders).
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_on_trade_qty(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                            double price, double quantity, uint8_t is_buy);

  // Feed a top-of-book snapshot (both best bid and best ask in one call).
  // For multi-level updates, build a BookUpdate on the C++ side; the C API
  // intentionally does not expose a stateful per-side helper because that
  // makes it too easy to accidentally clear the opposite side.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_on_best_levels(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                              double bid_price, double bid_qty, double ask_price,
                                              double ask_qty);

  // Feed a full L2 snapshot with parallel bid/ask arrays.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_simulated_executor_on_book_snapshot(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                                const double* bid_prices, const double* bid_qtys,
                                                uint32_t n_bids, const double* ask_prices,
                                                const double* ask_qtys, uint32_t n_asks);

  // BacktestResult handle: aggregates fills into trades + stats + equity curve.
  typedef void* FloxBacktestResultHandle;

  FLOX_EXPORT(group = "backtest_slippage")
  FloxBacktestResultHandle flox_backtest_result_create(double initial_capital,
                                                       double fee_rate,
                                                       uint8_t use_percentage_fee,
                                                       double fixed_fee_per_trade,
                                                       double risk_free_rate,
                                                       double annualization_factor);
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_backtest_result_destroy(FloxBacktestResultHandle result);

  // Feed fills produced by a SimulatedExecutor. Fills are processed in order.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_backtest_result_record_fill(FloxBacktestResultHandle result,
                                        uint64_t order_id, uint32_t symbol, uint8_t side,
                                        double price, double quantity, int64_t timestamp_ns);

  // Drain all fills from a SimulatedExecutor into a BacktestResult in FIFO order.
  FLOX_EXPORT(group = "backtest_slippage")
  void flox_backtest_result_ingest_executor(FloxBacktestResultHandle result,
                                            FloxSimulatedExecutorHandle executor);

  typedef struct
  {
    uint64_t totalTrades;
    uint64_t winningTrades;
    uint64_t losingTrades;
    uint64_t maxConsecutiveWins;
    uint64_t maxConsecutiveLosses;

    double initialCapital;
    double finalCapital;
    double totalPnl;
    double totalFees;
    double netPnl;
    double grossProfit;
    double grossLoss;

    double maxDrawdown;
    double maxDrawdownPct;

    double winRate;
    double profitFactor;
    double avgWin;
    double avgLoss;
    double avgWinLossRatio;

    double avgTradeDurationNs;
    double medianTradeDurationNs;
    double maxTradeDurationNs;

    double sharpeRatio;
    double sortinoRatio;
    double calmarRatio;
    double timeWeightedReturn;
    double returnPct;

    int64_t startTimeNs;
    int64_t endTimeNs;
  } FloxBacktestStats;

  FLOX_EXPORT(group = "backtest_slippage")
  void flox_backtest_result_stats(FloxBacktestResultHandle result, FloxBacktestStats* out);

  typedef struct
  {
    int64_t timestamp_ns;
    double equity;
    double drawdown_pct;
  } FloxEquityPoint;

  // Returns total available points. If points_out is non-NULL, writes up to
  // max_points entries and returns the number written.
  FLOX_EXPORT(group = "backtest_slippage")
  uint32_t flox_backtest_result_equity_curve(FloxBacktestResultHandle result,
                                             FloxEquityPoint* points_out,
                                             uint32_t max_points);

  FLOX_EXPORT(group = "backtest_slippage")
  uint8_t flox_backtest_result_write_equity_curve_csv(FloxBacktestResultHandle result,
                                                      const char* path);

  // Closed-trade record from a backtest. side: 0 = long (buy entry,
  // sell exit), 1 = short (sell entry, buy exit). Times are in
  // nanoseconds since epoch. Distinct from FloxTradeRecord (datareader),
  // which carries raw trade events from market data.
  typedef struct
  {
    uint32_t symbol;
    uint8_t side;
    double entry_price;
    double exit_price;
    double quantity;
    int64_t entry_time_ns;
    int64_t exit_time_ns;
    double pnl;
    double fee;
  } FloxBacktestTrade;

  // Returns total available trades. If trades_out is non-NULL, writes up to
  // max_trades entries and returns the number written.
  FLOX_EXPORT(group = "backtest_slippage")
  uint32_t flox_backtest_result_trades(FloxBacktestResultHandle result,
                                       FloxBacktestTrade* trades_out,
                                       uint32_t max_trades);

  // ============================================================
  // Segment operations (full API)
  // ============================================================

  typedef struct
  {
    uint8_t success;
    uint64_t segments_merged;
    uint64_t events_written;
    uint64_t bytes_written;
  } FloxMergeResult;

  typedef struct
  {
    uint8_t success;
    uint32_t segments_created;
    uint64_t events_written;
  } FloxSplitResult;

  typedef struct
  {
    uint8_t success;
    uint64_t events_exported;
    uint64_t bytes_written;
  } FloxExportResult;

  // merge: input_paths is \0-separated list of paths, total_len is combined length
  FLOX_EXPORT(group = "segment")
  FloxMergeResult flox_segment_merge_full(const char* input_paths, size_t num_paths,
                                          const char* output_dir, const char* output_name,
                                          uint8_t sort);

  FLOX_EXPORT(group = "segment")
  FloxMergeResult flox_segment_merge_dir(const char* input_dir, const char* output_dir);

  FLOX_EXPORT(group = "segment")
  FloxSplitResult flox_segment_split(const char* input_path, const char* output_dir,
                                     uint8_t mode, int64_t time_interval_ns,
                                     uint64_t events_per_file);

  FLOX_EXPORT(group = "segment")
  FloxExportResult flox_segment_export(const char* input_path, const char* output_path,
                                       uint8_t format, int64_t from_ns, int64_t to_ns,
                                       const uint32_t* symbols, uint32_t num_symbols);

  FLOX_EXPORT(group = "segment")
  uint8_t flox_segment_recompress(const char* input_path, const char* output_path,
                                  uint8_t compression);

  FLOX_EXPORT(group = "segment")
  uint64_t flox_segment_extract_symbols(const char* input_path, const char* output_path,
                                        const uint32_t* symbols, uint32_t num_symbols);

  FLOX_EXPORT(group = "segment")
  uint64_t flox_segment_extract_time_range(const char* input_path, const char* output_path,
                                           int64_t from_ns, int64_t to_ns);

  // ============================================================
  // Validation (full API)
  // ============================================================

  typedef struct
  {
    uint8_t valid;
    uint8_t header_valid;
    uint64_t reported_event_count;
    uint64_t actual_event_count;
    uint8_t has_index;
    uint8_t index_valid;
    uint64_t trades_found;
    uint64_t book_updates_found;
    uint32_t crc_errors;
    uint32_t timestamp_anomalies;
  } FloxSegmentValidation;

  FLOX_EXPORT(group = "validation")
  FloxSegmentValidation flox_segment_validate_full(const char* path, uint8_t verify_crc,
                                                   uint8_t verify_timestamps);

  typedef struct
  {
    uint8_t valid;
    uint32_t total_segments;
    uint32_t valid_segments;
    uint32_t corrupted_segments;
    uint64_t total_events;
    uint64_t total_bytes;
    int64_t first_timestamp;
    int64_t last_timestamp;
  } FloxDatasetValidation;

  FLOX_EXPORT(group = "validation")
  FloxDatasetValidation flox_dataset_validate(const char* data_dir);

  // ============================================================
  // DataReader (full API)
  // ============================================================

  typedef struct
  {
    int64_t first_event_ns;
    int64_t last_event_ns;
    uint64_t total_events;
    uint32_t segment_count;
    uint64_t total_bytes;
    double duration_seconds;
  } FloxDatasetSummary;

  FLOX_EXPORT(group = "datareader")
  FloxDataReaderHandle flox_data_reader_create_filtered(const char* data_dir, int64_t from_ns,
                                                        int64_t to_ns, const uint32_t* symbols,
                                                        uint32_t num_symbols);

  FLOX_EXPORT(group = "datareader")
  FloxDatasetSummary flox_data_reader_summary(FloxDataReaderHandle reader);

  typedef struct
  {
    uint64_t files_read;
    uint64_t events_read;
    uint64_t trades_read;
    uint64_t book_updates_read;
    uint64_t bytes_read;
    uint64_t crc_errors;
  } FloxReaderStats;

  FLOX_EXPORT(group = "datareader")
  FloxReaderStats flox_data_reader_stats(FloxDataReaderHandle reader);

  typedef struct
  {
    int64_t exchange_ts_ns;
    int64_t recv_ts_ns;
    int64_t price_raw;
    int64_t qty_raw;
    uint64_t trade_id;
    uint32_t symbol_id;
    uint8_t side;
  } FloxTradeRecord;

  // Returns number of trades read. If trades_out is NULL, counts only.
  FLOX_EXPORT(group = "datareader")
  uint64_t flox_data_reader_read_trades(FloxDataReaderHandle reader, FloxTradeRecord* trades_out,
                                        uint64_t max_trades);

  // Best bid/ask extracted from each book update event.
  typedef struct
  {
    int64_t exchange_ts_ns;
    int64_t recv_ts_ns;
    int64_t seq;
    int64_t bid_price_raw;
    int64_t bid_qty_raw;
    int64_t ask_price_raw;
    int64_t ask_qty_raw;
    uint32_t symbol_id;
    uint8_t event_type;  // 2=snapshot, 3=delta
  } FloxBBO;

  // Per-event header for read_book_updates(). Levels live in a separate
  // flat array; level_offset/bid_count/ask_count slice it for this event.
  typedef struct
  {
    int64_t exchange_ts_ns;
    int64_t recv_ts_ns;
    int64_t seq;
    uint64_t level_offset;
    uint32_t symbol_id;
    uint16_t bid_count;
    uint16_t ask_count;
    uint8_t event_type;  // 2=snapshot, 3=delta
  } FloxBookUpdateHeader;

  typedef struct
  {
    int64_t price_raw;
    int64_t qty_raw;
    uint8_t side;  // 0=bid, 1=ask
  } FloxLevel;

  // Returns number of book update events read. If bbos_out is NULL, counts only.
  FLOX_EXPORT(group = "datareader")
  uint64_t flox_data_reader_read_bbo(FloxDataReaderHandle reader, FloxBBO* bbos_out,
                                     uint64_t max_events);

  // Counts book update events and total levels in a single pass.
  // Returns event count; writes total level count to *total_levels_out (may be NULL).
  FLOX_EXPORT(group = "datareader")
  uint64_t flox_data_reader_count_book_updates(FloxDataReaderHandle reader,
                                               uint64_t* total_levels_out);

  // Reads book updates into pre-sized arrays. Caller must size headers_out
  // (>= event count) and levels_out (>= total levels) using
  // flox_data_reader_count_book_updates().
  // Returns number of events read.
  FLOX_EXPORT(group = "datareader")
  uint64_t flox_data_reader_read_book_updates(FloxDataReaderHandle reader,
                                              FloxBookUpdateHeader* headers_out,
                                              uint64_t max_events,
                                              FloxLevel* levels_out,
                                              uint64_t max_levels);

  // Mid-stream seek variants. Each starts iteration from the first event whose
  // exchange_ts_ns >= start_ts_ns and otherwise behaves like the matching
  // non-_from reader. Useful when you keep one reader open and want to pull
  // batches from arbitrary timestamps without re-opening segments. If
  // start_ts_ns falls before the dataset's first event, behaves like the
  // non-_from variant.

  FLOX_EXPORT(group = "datareader")
  uint64_t flox_data_reader_read_trades_from(FloxDataReaderHandle reader,
                                             int64_t start_ts_ns,
                                             FloxTradeRecord* trades_out,
                                             uint64_t max_trades);

  FLOX_EXPORT(group = "datareader")
  uint64_t flox_data_reader_read_bbo_from(FloxDataReaderHandle reader,
                                          int64_t start_ts_ns,
                                          FloxBBO* bbos_out,
                                          uint64_t max_events);

  FLOX_EXPORT(group = "datareader")
  uint64_t flox_data_reader_count_book_updates_from(FloxDataReaderHandle reader,
                                                    int64_t start_ts_ns,
                                                    uint64_t* total_levels_out);

  FLOX_EXPORT(group = "datareader")
  uint64_t flox_data_reader_read_book_updates_from(FloxDataReaderHandle reader,
                                                   int64_t start_ts_ns,
                                                   FloxBookUpdateHeader* headers_out,
                                                   uint64_t max_events,
                                                   FloxLevel* levels_out,
                                                   uint64_t max_levels);

  // ============================================================
  // MergedTapeReader — read N `.floxlog` directories as one merged
  // stream, sorted by exchange_ts_ns. Symbols are rekeyed into a global
  // id space keyed by (metadata.exchange, name). Trades may overlap in
  // time across tapes; overlapping book streams for the same
  // (exchange, name) raise on construction (returns NULL handle and
  // sets the FloxError thread-local). See docs/errors/E_INPUT_003.md.
  // ============================================================

  typedef void* FloxMergedTapeReaderHandle;

  // Metadata about one global symbol after rekey. Strings are owned by
  // the reader — valid until the reader is destroyed.
  typedef struct
  {
    uint32_t global_id;
    int8_t price_precision;
    int8_t qty_precision;
    uint8_t _pad[2];
    const char* exchange;  // borrowed pointer
    const char* name;      // borrowed pointer
  } FloxMergedSymbol;

  // Per-tape contribution counts. Path is borrowed.
  typedef struct
  {
    int64_t first_event_ns;
    int64_t last_event_ns;
    uint64_t trades;
    uint64_t books;
    const char* path;
  } FloxMergedTapeStats;

  FLOX_EXPORT(group = "merged_tape_reader")
  FloxMergedTapeReaderHandle
  flox_merged_tape_reader_create(const char* const* paths, uint32_t n_paths,
                                 int64_t from_ns,  // -1 = no lower bound
                                 int64_t to_ns,    // -1 = no upper bound
                                 const uint32_t* symbol_filter,
                                 uint32_t n_filter);

  FLOX_EXPORT(group = "merged_tape_reader")
  void flox_merged_tape_reader_destroy(FloxMergedTapeReaderHandle reader);

  FLOX_EXPORT(group = "merged_tape_reader")
  uint32_t flox_merged_tape_reader_symbol_count(FloxMergedTapeReaderHandle reader);

  // Populate `out` with up to `max` symbols. Returns the number written.
  // Pass NULL out to size — returns the actual count.
  FLOX_EXPORT(group = "merged_tape_reader")
  uint32_t flox_merged_tape_reader_get_symbols(FloxMergedTapeReaderHandle reader,
                                               FloxMergedSymbol* out,
                                               uint32_t max);

  FLOX_EXPORT(group = "merged_tape_reader")
  uint32_t flox_merged_tape_reader_tape_count(FloxMergedTapeReaderHandle reader);

  FLOX_EXPORT(group = "merged_tape_reader")
  uint32_t flox_merged_tape_reader_get_tape_stats(FloxMergedTapeReaderHandle reader,
                                                  FloxMergedTapeStats* out,
                                                  uint32_t max);

  FLOX_EXPORT(group = "merged_tape_reader")
  void flox_merged_tape_reader_time_range(FloxMergedTapeReaderHandle reader,
                                          int64_t* min_first_ns_out,
                                          int64_t* max_last_ns_out);

  // Two-phase trade read: count first, then allocate, then fill.
  FLOX_EXPORT(group = "merged_tape_reader")
  uint64_t flox_merged_tape_reader_count_trades(FloxMergedTapeReaderHandle reader);

  // FloxTradeRecord matches the reader's struct (datareader group);
  // symbol_id field carries the global_id.
  FLOX_EXPORT(group = "merged_tape_reader")
  uint64_t flox_merged_tape_reader_read_trades(FloxMergedTapeReaderHandle reader,
                                               FloxTradeRecord* trades_out,
                                               uint64_t max_trades);

  // Books — same shape as flox_data_reader_count_book_updates /
  // _read_book_updates.
  FLOX_EXPORT(group = "merged_tape_reader")
  uint64_t flox_merged_tape_reader_count_books(FloxMergedTapeReaderHandle reader,
                                               uint64_t* total_levels_out);

  FLOX_EXPORT(group = "merged_tape_reader")
  uint64_t flox_merged_tape_reader_read_books(FloxMergedTapeReaderHandle reader,
                                              FloxBookUpdateHeader* headers_out,
                                              uint64_t max_events,
                                              FloxLevel* levels_out,
                                              uint64_t max_levels);

  // ============================================================
  // DataWriter (extras)
  // ============================================================

  typedef struct
  {
    uint64_t bytes_written;
    uint64_t events_written;
    uint64_t segments_created;
    uint64_t trades_written;
  } FloxWriterStats;

  FLOX_EXPORT(group = "datawriter")
  FloxWriterStats flox_data_writer_stats(FloxDataWriterHandle writer);

  // Batched book writer. headers + flat levels array, sliced per event
  // via header.level_offset / bid_count / ask_count. Same struct layout
  // as flox_data_reader_read_book_updates, so round-trip works.
  // event_type 2=snapshot, 3=delta. FloxLevel.side is ignored on write
  // (bid vs ask is derived from the bid_count/ask_count split). Returns
  // count successfully written.
  FLOX_EXPORT(group = "datawriter")
  uint64_t flox_data_writer_write_books(FloxDataWriterHandle writer,
                                        const FloxBookUpdateHeader* headers,
                                        uint64_t n_events,
                                        const FloxLevel* levels,
                                        uint64_t total_levels);

  // ============================================================
  // Partitioner
  // ============================================================

  typedef void* FloxPartitionerHandle;

  typedef struct
  {
    uint32_t partition_id;
    int64_t from_ns;
    int64_t to_ns;
    int64_t warmup_from_ns;
    uint64_t estimated_events;
    uint64_t estimated_bytes;
  } FloxPartition;

  FLOX_EXPORT(group = "partitioner")
  FloxPartitionerHandle flox_partitioner_create(const char* data_dir);
  FLOX_EXPORT(group = "partitioner")
  void flox_partitioner_destroy(FloxPartitionerHandle partitioner);

  // All partition functions return number of partitions.
  // If partitions_out is NULL, returns count only.
  FLOX_EXPORT(group = "partitioner")
  uint32_t flox_partitioner_by_time(FloxPartitionerHandle p, uint32_t num_partitions,
                                    int64_t warmup_ns, FloxPartition* partitions_out,
                                    uint32_t max_partitions);
  FLOX_EXPORT(group = "partitioner")
  uint32_t flox_partitioner_by_duration(FloxPartitionerHandle p, int64_t duration_ns,
                                        int64_t warmup_ns, FloxPartition* partitions_out,
                                        uint32_t max_partitions);
  FLOX_EXPORT(group = "partitioner")
  uint32_t flox_partitioner_by_calendar(FloxPartitionerHandle p, uint8_t unit,
                                        int64_t warmup_ns, FloxPartition* partitions_out,
                                        uint32_t max_partitions);
  FLOX_EXPORT(group = "partitioner")
  uint32_t flox_partitioner_by_symbol(FloxPartitionerHandle p, uint32_t num_partitions,
                                      FloxPartition* partitions_out, uint32_t max_partitions);
  FLOX_EXPORT(group = "partitioner")
  uint32_t flox_partitioner_per_symbol(FloxPartitionerHandle p, FloxPartition* partitions_out,
                                       uint32_t max_partitions);
  FLOX_EXPORT(group = "partitioner")
  uint32_t flox_partitioner_by_event_count(FloxPartitionerHandle p, uint32_t num_partitions,
                                           FloxPartition* partitions_out,
                                           uint32_t max_partitions);

  // ============================================================
  // Pointer-out wrappers for struct-returning functions.
  // These exist for language bindings (Codon, QuickJS) that cannot
  // consume C structs returned by value via their FFI.
  // Each writes sizeof(OriginalStruct) bytes to *out.
  // ============================================================

  FLOX_EXPORT(group = "pointer_out")
  void flox_data_reader_summary_p(FloxDataReaderHandle reader, void* out);
  FLOX_EXPORT(group = "pointer_out")
  void flox_data_reader_stats_p(FloxDataReaderHandle reader, void* out);
  FLOX_EXPORT(group = "pointer_out")
  void flox_data_writer_stats_p(FloxDataWriterHandle writer, void* out);
  FLOX_EXPORT(group = "pointer_out")
  void flox_binary_log_recorder_hook_stats_p(void* hook, void* out);
  FLOX_EXPORT(group = "pointer_out")
  void flox_segment_merge_full_p(const char* input_paths, size_t num_paths,
                                 const char* output_dir, const char* output_name,
                                 uint8_t sort, void* out);
  FLOX_EXPORT(group = "pointer_out")
  void flox_segment_merge_dir_p(const char* input_dir, const char* output_dir, void* out);
  FLOX_EXPORT(group = "pointer_out")
  void flox_segment_split_p(const char* input_path, const char* output_dir, uint8_t mode,
                            int64_t time_interval_ns, uint64_t events_per_file, void* out);
  FLOX_EXPORT(group = "pointer_out")
  void flox_segment_export_p(const char* input_path, const char* output_path, uint8_t format,
                             int64_t from_ns, int64_t to_ns,
                             const uint32_t* symbols, uint32_t num_symbols, void* out);
  FLOX_EXPORT(group = "pointer_out")
  void flox_segment_validate_full_p(const char* path, uint8_t verify_crc,
                                    uint8_t verify_timestamps, void* out);
  FLOX_EXPORT(group = "pointer_out")
  void flox_dataset_validate_p(const char* data_dir, void* out);

  // ============================================================
  // Signal type — emitted by strategies, received by order backends.
  // Shared by FloxLiveEngine and StrategyRunner.
  // ============================================================

  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol;
    uint8_t side;        // 0=buy, 1=sell
    uint8_t order_type;  // 0=market, 1=limit, 2=stop_market, 3=stop_limit,
                         // 4=tp_market, 5=tp_limit, 6=trailing_stop,
                         // 7=cancel, 8=cancel_all, 9=modify
    double price;        // limit price (0 for market orders)
    double quantity;
    double trigger_price;    // stop/take-profit trigger
    double trailing_offset;  // trailing stop — absolute price offset
    int32_t trailing_bps;    // trailing stop — callback rate in basis points
    double new_price;        // modify: updated price
    double new_quantity;     // modify: updated quantity
  } FloxSignal;

  typedef void (*FloxOnSignalCallback)(void* user_data, const FloxSignal* signal);

  // ============================================================
  // RiskManager — pre-trade hook callable from runner / live engine.
  //
  // The `allow` callback is invoked synchronously on every signal a
  // strategy emits, *before* the user's on_signal callback fires. Returning
  // 0 (deny) drops the signal entirely; returning 1 (allow) lets it
  // propagate. Use this to enforce risk limits, kill switches, or
  // jurisdiction-specific rules without modifying the strategy itself.
  //
  // The signal pointer passed to `allow` aliases the same FloxSignal that
  // would otherwise be delivered to on_signal — fields are read-only;
  // mutations are not propagated.
  //
  // Lifecycle: created via flox_risk_manager_create, attached to a
  // runner/engine via flox_runner_set_risk_manager /
  // flox_live_engine_set_risk_manager (NULL to detach), destroyed via
  // flox_risk_manager_destroy. The handle may be shared across multiple
  // runners/engines; the caller owns destruction.
  // ============================================================

  typedef uint8_t (*FloxRiskManagerAllowFn)(void* user_data,
                                            const FloxSignal* signal);

  typedef struct
  {
    FloxRiskManagerAllowFn allow;
    void* user_data;
  } FloxRiskManagerCallbacks;

  typedef void* FloxRiskManagerHandle;

  FLOX_EXPORT(group = "risk")
  FloxRiskManagerHandle flox_risk_manager_create(FloxRiskManagerCallbacks callbacks);
  FLOX_EXPORT(group = "risk")
  FloxRiskManagerHandle flox_risk_manager_create_p(const FloxRiskManagerCallbacks* callbacks);
  FLOX_EXPORT(group = "risk")
  void flox_risk_manager_destroy(FloxRiskManagerHandle rm);

  // ============================================================
  // KillSwitch — global halt hook. Fires before OrderValidator and
  // RiskManager (cheap check first); when `check` returns 0, the signal
  // is dropped and downstream hooks are skipped.
  //
  // Use this for "trading halted, no orders allowed" semantics. The
  // binding manages the trigger state itself; the C API just asks via
  // the callback. NULL `check` is a no-op.
  // ============================================================

  typedef uint8_t (*FloxKillSwitchCheckFn)(void* user_data,
                                           const FloxSignal* signal);

  typedef struct
  {
    FloxKillSwitchCheckFn check;
    void* user_data;
  } FloxKillSwitchCallbacks;

  typedef void* FloxKillSwitchHandle;

  FLOX_EXPORT(group = "risk")
  FloxKillSwitchHandle flox_kill_switch_create(FloxKillSwitchCallbacks callbacks);
  FLOX_EXPORT(group = "risk")
  FloxKillSwitchHandle flox_kill_switch_create_p(const FloxKillSwitchCallbacks* callbacks);
  FLOX_EXPORT(group = "risk")
  void flox_kill_switch_destroy(FloxKillSwitchHandle ks);

  // ============================================================
  // OrderValidator — sanity check. Fires after KillSwitch and before
  // RiskManager. When `validate` returns 0, the signal is dropped and
  // RiskManager is skipped.
  //
  // Use this to catch malformed orders (qty <= 0, missing price on a
  // limit, etc.) before any business-logic evaluation. NULL `validate`
  // is a no-op.
  // ============================================================

  typedef uint8_t (*FloxOrderValidatorValidateFn)(void* user_data,
                                                  const FloxSignal* signal);

  typedef struct
  {
    FloxOrderValidatorValidateFn validate;
    void* user_data;
  } FloxOrderValidatorCallbacks;

  typedef void* FloxOrderValidatorHandle;

  FLOX_EXPORT(group = "risk")
  FloxOrderValidatorHandle flox_order_validator_create(FloxOrderValidatorCallbacks callbacks);
  FLOX_EXPORT(group = "risk")
  FloxOrderValidatorHandle flox_order_validator_create_p(const FloxOrderValidatorCallbacks* callbacks);
  FLOX_EXPORT(group = "risk")
  void flox_order_validator_destroy(FloxOrderValidatorHandle ov);

  // ============================================================
  // Logger — process-wide log redirection callback.
  //
  // FLOX_LOG_INFO / WARN / ERROR macros normally write to stderr via
  // the bundled ConsoleLogger. Setting a callback here redirects every
  // subsequent log call to the binding's language-native logger
  // (Python `logging`, Node `console`, etc.).
  //
  // `level`: 0 = Info, 1 = Warn, 2 = Error.
  // `msg`:   null-terminated UTF-8 string. Valid only for the duration
  //          of the callback; copy if retained.
  //
  // Single global setter — no per-engine isolation, by design (logs
  // are a process concern). Pass NULL to restore the default
  // ConsoleLogger.
  //
  // Thread-safe: the global pointer is atomic; callbacks may fire from
  // any consumer thread. The user-supplied callback must therefore be
  // thread-safe.
  // ============================================================

  typedef void (*FloxLogCallback)(void* user_data, int32_t level, const char* msg);

  FLOX_EXPORT(group = "logger")
  void flox_set_log_callback(FloxLogCallback callback, void* user_data);

  // ============================================================
  // PnLTracker — post-emission observer hook.
  //
  // Unlike the pre-trade gates (KillSwitch / OrderValidator /
  // RiskManager) which can DROP a signal, PnLTracker fires **after**
  // the user's on_signal callback runs. It's an observer; the return
  // type is void and the signal has already been delivered to the
  // binding. Use this for shadow-tracking exposure based on emitted
  // signals — independent of any later fill confirmation from a
  // broker.
  //
  // For real fill-driven P&L, attach the binding's tracker to its
  // own broker callback path; the C API doesn't surface fill events
  // beyond the SimulatedExecutor.
  //
  // Lifecycle / threading match RiskManager: NULL detaches; atomic
  // pointer swap is safe with consumer threads active.
  // ============================================================

  typedef void (*FloxPnLTrackerOnSignalFn)(void* user_data, const FloxSignal* signal);

  typedef struct
  {
    FloxPnLTrackerOnSignalFn on_signal;
    void* user_data;
  } FloxPnLTrackerCallbacks;

  typedef void* FloxPnLTrackerHandle;

  FLOX_EXPORT(group = "metrics")
  FloxPnLTrackerHandle flox_pnl_tracker_create(FloxPnLTrackerCallbacks callbacks);
  FLOX_EXPORT(group = "metrics")
  FloxPnLTrackerHandle flox_pnl_tracker_create_p(const FloxPnLTrackerCallbacks* callbacks);
  FLOX_EXPORT(group = "metrics")
  void flox_pnl_tracker_destroy(FloxPnLTrackerHandle tracker);

  // ============================================================
  // StorageSink — persist every emitted signal.
  //
  // Same shape and timing as PnLTracker — fires after on_signal,
  // never blocks. Use this to write each emitted signal to the
  // binding's storage of choice (DB, append-only log, broker audit
  // trail).
  //
  // The signal pointer is read-only and valid only for the duration
  // of the callback; copy if retained.
  // ============================================================

  typedef void (*FloxStorageSinkStoreFn)(void* user_data, const FloxSignal* signal);

  typedef struct
  {
    FloxStorageSinkStoreFn store;
    void* user_data;
  } FloxStorageSinkCallbacks;

  typedef void* FloxStorageSinkHandle;

  FLOX_EXPORT(group = "storage")
  FloxStorageSinkHandle flox_storage_sink_create(FloxStorageSinkCallbacks callbacks);
  FLOX_EXPORT(group = "storage")
  FloxStorageSinkHandle flox_storage_sink_create_p(const FloxStorageSinkCallbacks* callbacks);
  FLOX_EXPORT(group = "storage")
  void flox_storage_sink_destroy(FloxStorageSinkHandle sink);

  // ============================================================
  // MarketDataRecorder — receive every market data event fed into the
  // engine, for custom recording (CSV, parquet, custom binary).
  //
  // Two flavours share the same FloxMarketDataRecorderHandle plug
  // socket on `flox_runner_set_market_data_recorder` and
  // `flox_live_engine_set_market_data_recorder`:
  //   1. User-callback hook (`flox_market_data_recorder_create`). The
  //      binding implements on_trade / on_book_update etc. in the host
  //      language.
  //   2. Binary-log sink (`flox_binary_log_recorder_hook_create`).
  //      Built-in `.floxlog` writer; events stay in C++ on the hot
  //      path and are written via `BinaryLogWriter` without crossing
  //      the binding boundary.
  //
  // Callbacks fire on every published trade and book update:
  // synchronously for the runner, on the consumer thread for the live
  // engine. on_start / on_stop fire on engine.start() / engine.stop()
  // while the recorder is attached. Any callback may be NULL (no-op).
  //
  // The trade / book pointers and book level arrays are valid only for
  // the duration of the callback; copy if retained.
  // ============================================================

  typedef void (*FloxRecorderOnTradeFn)(void* user_data, const FloxTradeData* trade);
  typedef void (*FloxRecorderOnBookUpdateFn)(void* user_data,
                                             uint32_t symbol,
                                             uint8_t is_snapshot,
                                             const FloxBookLevel* bids,
                                             uint32_t n_bids,
                                             const FloxBookLevel* asks,
                                             uint32_t n_asks,
                                             int64_t exchange_ts_ns);
  typedef void (*FloxRecorderLifecycleFn)(void* user_data);

  typedef struct
  {
    FloxRecorderOnTradeFn on_trade;
    FloxRecorderOnBookUpdateFn on_book_update;
    FloxRecorderLifecycleFn on_start;
    FloxRecorderLifecycleFn on_stop;
    void* user_data;
  } FloxMarketDataRecorderCallbacks;

  typedef void* FloxMarketDataRecorderHandle;

  FLOX_EXPORT(group = "recorder")
  FloxMarketDataRecorderHandle
  flox_market_data_recorder_create(FloxMarketDataRecorderCallbacks callbacks);
  FLOX_EXPORT(group = "recorder")
  FloxMarketDataRecorderHandle
  flox_market_data_recorder_create_p(const FloxMarketDataRecorderCallbacks* callbacks);
  FLOX_EXPORT(group = "recorder")
  void flox_market_data_recorder_destroy(FloxMarketDataRecorderHandle recorder);

  // Binary-log recorder hook. Owns a BinaryLogWriter internally and
  // writes trades + books on the engine thread without crossing the
  // binding boundary. `compression` matches
  // `flox::replay::CompressionType`: 0 = None, 1 = LZ4.
  typedef void* FloxBinaryLogRecorderHookHandle;

  FLOX_EXPORT(group = "binary_log_recorder_hook")
  FloxBinaryLogRecorderHookHandle
  flox_binary_log_recorder_hook_create(const char* output_dir,
                                       uint64_t max_segment_mb,
                                       uint8_t exchange_id,
                                       uint8_t compression);

  // Variant that stamps RecordingMetadata.exchange / instrument_type
  // into the tape manifest. MergedTapeReader keys tapes by
  // (exchange, name), so any binding that writes tapes that need to be
  // merge-readable must call this form. Pass NULL or "" for either
  // string to leave it empty.
  FLOX_EXPORT(group = "binary_log_recorder_hook")
  FloxBinaryLogRecorderHookHandle
  flox_binary_log_recorder_hook_create_ex(const char* output_dir,
                                          uint64_t max_segment_mb,
                                          uint8_t exchange_id,
                                          uint8_t compression,
                                          const char* exchange_name,
                                          const char* instrument_type);

  FLOX_EXPORT(group = "binary_log_recorder_hook")
  void flox_binary_log_recorder_hook_destroy(FloxBinaryLogRecorderHookHandle hook);

  // Borrowed handle compatible with flox_runner_set_market_data_recorder
  // and flox_live_engine_set_market_data_recorder. Lifetime is tied to
  // the owning hook. DO NOT pass to flox_market_data_recorder_destroy.
  FLOX_EXPORT(group = "binary_log_recorder_hook")
  FloxMarketDataRecorderHandle
  flox_binary_log_recorder_hook_as_recorder(FloxBinaryLogRecorderHookHandle hook);

  FLOX_EXPORT(group = "binary_log_recorder_hook")
  void flox_binary_log_recorder_hook_add_symbol(FloxBinaryLogRecorderHookHandle hook,
                                                uint32_t symbol_id,
                                                const char* name,
                                                const char* base,
                                                const char* quote,
                                                int8_t price_precision,
                                                int8_t qty_precision);

  FLOX_EXPORT(group = "binary_log_recorder_hook")
  void flox_binary_log_recorder_hook_flush(FloxBinaryLogRecorderHookHandle hook);

  FLOX_EXPORT(group = "binary_log_recorder_hook")
  FloxWriterStats
  flox_binary_log_recorder_hook_stats(FloxBinaryLogRecorderHookHandle hook);

  // ============================================================
  // ReplaySource — binding-side market data event source.
  //
  // Bindings provide a custom event reader (e.g. exchange-specific
  // archive format, S3-streamed segments) that BacktestRunner pulls
  // events from. The engine drives playback by repeatedly calling
  // `next` until it returns 0.
  //
  // Event shape: `next` populates a FloxReplayEvent describing one
  // event (Trade, BookSnapshot, or BookDelta). For book events, the
  // binding sets `bids` / `asks` to point at its own buffer; the
  // pointers must remain valid until the next `next` call. The engine
  // copies the levels it needs before invoking the callback again.
  // ============================================================

  // 1=Trade, 2=BookSnapshot, 3=BookDelta. Matches flox::replay::EventType.
  typedef struct
  {
    uint8_t type;
    uint8_t _pad[3];
    int64_t timestamp_ns;

    // Trade payload — valid only when type == 1.
    uint32_t trade_symbol;
    uint8_t trade_is_buy;
    uint8_t _pad2[3];
    int64_t trade_price_raw;
    int64_t trade_quantity_raw;

    // Book payload — valid when type == 2 or 3.
    uint32_t book_symbol;
    uint32_t n_bids;
    uint32_t n_asks;
    uint32_t _pad3;
    // Pointers owned by the binding; must remain valid for the duration
    // of the engine's processing of this event (until the next `next`
    // call returns).
    const FloxBookLevel* bids;
    const FloxBookLevel* asks;
  } FloxReplayEvent;

  // Yield the next event. Return 1 if an event was produced (event_out
  // populated), 0 if the source is exhausted.
  typedef uint8_t (*FloxReplaySourceNextFn)(void* user_data, FloxReplayEvent* event_out);
  // Seek to a timestamp. Return 1 if the seek succeeded, 0 otherwise.
  typedef uint8_t (*FloxReplaySourceSeekFn)(void* user_data, int64_t timestamp_ns);
  typedef void (*FloxReplaySourceLifecycleFn)(void* user_data);

  typedef struct
  {
    FloxReplaySourceLifecycleFn on_start;
    FloxReplaySourceLifecycleFn on_stop;
    FloxReplaySourceSeekFn seek_to;
    FloxReplaySourceNextFn next;
    void* user_data;
  } FloxReplaySourceCallbacks;

  typedef void* FloxReplaySourceHandle;

  FLOX_EXPORT(group = "replay")
  FloxReplaySourceHandle flox_replay_source_create(FloxReplaySourceCallbacks callbacks);
  FLOX_EXPORT(group = "replay")
  FloxReplaySourceHandle flox_replay_source_create_p(const FloxReplaySourceCallbacks* callbacks);
  FLOX_EXPORT(group = "replay")
  void flox_replay_source_destroy(FloxReplaySourceHandle source);

  // Seek the underlying source to a timestamp. Returns 1 on success.
  // Convenience wrapper that forwards to the binding's seek_to callback.
  FLOX_EXPORT(group = "replay")
  uint8_t flox_replay_source_seek_to(FloxReplaySourceHandle source, int64_t timestamp_ns);

  // ============================================================
  // ExecutionListener — observe order lifecycle events.
  //
  // Bindings register callbacks that fire as orders move through the
  // execution path: submitted → accepted → (partially) filled / canceled
  // / rejected / expired / replaced. Used as a backtest observer (the
  // SimulatedExecutor inside BacktestRunner emits these events) or to
  // surface live-broker fills back into binding code.
  //
  // The order pointer is read-only and valid only for the duration of
  // the callback; copy the fields you need to retain.
  // Any callback may be NULL (no-op).
  // ============================================================

  // ABI-stable order snapshot, mirrors flox::Order. Raw fields are
  // fixed-point int64 (Price * 1e8, Quantity * 1e8) — matches the rest
  // of the C API.
  typedef struct
  {
    uint64_t id;
    uint64_t client_order_id;
    uint32_t symbol;
    uint16_t strategy_id;
    uint16_t order_tag;
    uint8_t side;           // 0=BUY, 1=SELL
    uint8_t type;           // OrderType enum (0=LIMIT, 1=MARKET, ...)
    uint8_t time_in_force;  // TimeInForce enum
    uint8_t flags;          // ExecutionFlags packed bits
    int64_t price_raw;
    int64_t quantity_raw;
    int64_t filled_quantity_raw;
    int64_t trigger_price_raw;
    int64_t trailing_offset_raw;
    int64_t created_at_ns;
    int64_t exchange_ts_ns;
  } FloxOrder;

  typedef void (*FloxExecListenerOnOrderFn)(void* user_data, const FloxOrder* order);
  typedef void (*FloxExecListenerOnPartialFillFn)(void* user_data,
                                                  const FloxOrder* order,
                                                  int64_t fill_quantity_raw);
  typedef void (*FloxExecListenerOnRejectedFn)(void* user_data,
                                               const FloxOrder* order,
                                               const char* reason);
  typedef void (*FloxExecListenerOnReplacedFn)(void* user_data,
                                               const FloxOrder* old_order,
                                               const FloxOrder* new_order);
  typedef void (*FloxExecListenerOnTrailingUpdateFn)(void* user_data,
                                                     const FloxOrder* order,
                                                     int64_t new_trigger_price_raw);
  typedef void (*FloxExecListenerOnQueuePositionChangeFn)(void* user_data,
                                                          const FloxOrder* order,
                                                          int64_t queue_ahead_raw,
                                                          int64_t queue_total_raw);
  typedef void (*FloxExecListenerOnMarketPositionChangeFn)(void* user_data,
                                                           const FloxOrder* order,
                                                           uint8_t market_position,
                                                           int32_t distance_to_best_ticks);
  typedef void (*FloxExecListenerOnReplaceSubmittedFn)(void* user_data,
                                                       const FloxOrder* old_order,
                                                       const FloxOrder* new_order);
  typedef void (*FloxExecListenerOnReplaceAcceptedFn)(void* user_data,
                                                      const FloxOrder* old_order,
                                                      const FloxOrder* new_order);
  typedef void (*FloxExecListenerOnReplaceRejectedFn)(void* user_data,
                                                      const FloxOrder* old_order,
                                                      const FloxOrder* new_order,
                                                      const char* reason);

  typedef struct
  {
    FloxExecListenerOnOrderFn on_submitted;
    FloxExecListenerOnOrderFn on_accepted;
    FloxExecListenerOnPartialFillFn on_partially_filled;
    FloxExecListenerOnOrderFn on_filled;
    FloxExecListenerOnOrderFn on_pending_cancel;
    FloxExecListenerOnOrderFn on_canceled;
    FloxExecListenerOnOrderFn on_expired;
    FloxExecListenerOnRejectedFn on_rejected;
    FloxExecListenerOnReplacedFn on_replaced;
    FloxExecListenerOnOrderFn on_pending_trigger;
    FloxExecListenerOnOrderFn on_triggered;
    FloxExecListenerOnTrailingUpdateFn on_trailing_stop_updated;
    FloxExecListenerOnQueuePositionChangeFn on_queue_position_change;
    FloxExecListenerOnMarketPositionChangeFn on_market_position_change;
    FloxExecListenerOnReplaceSubmittedFn on_replace_submitted;
    FloxExecListenerOnReplaceAcceptedFn on_replace_accepted;
    FloxExecListenerOnReplaceRejectedFn on_replace_rejected;
    void* user_data;
  } FloxExecutionListenerCallbacks;

  typedef void* FloxExecutionListenerHandle;

  FLOX_EXPORT(group = "execution")
  FloxExecutionListenerHandle
  flox_execution_listener_create(FloxExecutionListenerCallbacks callbacks);
  FLOX_EXPORT(group = "execution")
  FloxExecutionListenerHandle
  flox_execution_listener_create_p(const FloxExecutionListenerCallbacks* callbacks);
  FLOX_EXPORT(group = "execution")
  void flox_execution_listener_destroy(FloxExecutionListenerHandle listener);

  // Attaching listeners to BacktestRunner is declared with the rest of
  // the BacktestRunner API below (after FloxBacktestRunnerHandle).

  // ============================================================
  // Executor — binding-supplied IOrderExecutor.
  //
  // Bindings provide an executor that places, cancels, replaces and
  // OCO-submits orders on a real broker (or on a custom simulator). The
  // engine routes every signal through this executor instead of the
  // built-in SimulatedExecutor when one is attached.
  //
  // The concrete in-process SimulatedExecutor is exposed separately as
  // flox_simulated_executor_*.
  //
  // The order pointer is read-only and valid only for the duration of
  // the callback; copy fields you need to retain. Any callback may be
  // NULL (no-op).
  // ============================================================

  // Mirror of flox::ExchangeCapabilities. Bindings fill this in their
  // capabilities() callback so the engine knows which order types,
  // time-in-force values and execution flags the venue supports.
  typedef struct
  {
    uint8_t supports_stop_market;
    uint8_t supports_stop_limit;
    uint8_t supports_take_profit_market;
    uint8_t supports_take_profit_limit;
    uint8_t supports_trailing_stop;
    uint8_t supports_iceberg;
    uint8_t supports_oco;
    uint8_t supports_gtc;
    uint8_t supports_ioc;
    uint8_t supports_fok;
    uint8_t supports_gtd;
    uint8_t supports_post_only;
    uint8_t supports_reduce_only;
    uint8_t supports_close_position;
    uint8_t _pad[2];
  } FloxExchangeCapabilities;

  typedef void (*FloxExecutorSubmitFn)(void* user_data, const FloxOrder* order);
  typedef void (*FloxExecutorCancelFn)(void* user_data, uint64_t order_id);
  typedef void (*FloxExecutorCancelAllFn)(void* user_data, uint32_t symbol);
  typedef void (*FloxExecutorReplaceFn)(void* user_data,
                                        uint64_t old_order_id,
                                        const FloxOrder* new_order);
  typedef void (*FloxExecutorSubmitOCOFn)(void* user_data,
                                          const FloxOrder* order1,
                                          const FloxOrder* order2);
  typedef void (*FloxExecutorCapabilitiesFn)(void* user_data,
                                             FloxExchangeCapabilities* caps_out);
  typedef void (*FloxExecutorLifecycleFn)(void* user_data);

  typedef struct
  {
    FloxExecutorSubmitFn submit;
    FloxExecutorCancelFn cancel;
    FloxExecutorCancelAllFn cancel_all;
    FloxExecutorReplaceFn replace;
    FloxExecutorSubmitOCOFn submit_oco;
    FloxExecutorCapabilitiesFn capabilities;
    FloxExecutorLifecycleFn on_start;
    FloxExecutorLifecycleFn on_stop;
    void* user_data;
  } FloxExecutorCallbacks;

  typedef void* FloxExecutorHandle;

  FLOX_EXPORT(group = "execution")
  FloxExecutorHandle flox_executor_create(FloxExecutorCallbacks callbacks);
  FLOX_EXPORT(group = "execution")
  FloxExecutorHandle flox_executor_create_p(const FloxExecutorCallbacks* callbacks);
  FLOX_EXPORT(group = "execution")
  void flox_executor_destroy(FloxExecutorHandle executor);

  // Query the executor's reported capabilities. Forwards to the binding's
  // capabilities() callback. caps_out is zeroed if no callback registered.
  FLOX_EXPORT(group = "execution")
  void flox_executor_get_capabilities(FloxExecutorHandle executor,
                                      FloxExchangeCapabilities* caps_out);

  // ============================================================
  // FloxLiveEngine — Disruptor-based live trading engine.
  //
  // Uses real EventBus (SPSC ring buffer / Disruptor) internally.
  // Each subscribed strategy runs in its own bus consumer thread.
  // Publish functions are called from the caller's thread (e.g., Python
  // asyncio, Node.js event loop, Codon main thread) and are lock-free.
  //
  // Usage:
  //   1. Create a registry and register symbols.
  //   2. Create a live engine.
  //   3. For each strategy: create via flox_strategy_create, then
  //      call flox_live_engine_add_strategy (assigns a signal handler).
  //   4. flox_live_engine_start() — spawns consumer threads.
  //   5. Push market data: flox_live_engine_publish_trade / book_snapshot.
  //      Returns immediately; strategy callbacks fire in consumer threads.
  //   6. flox_live_engine_stop() — drains buses, joins threads.
  // ============================================================

  typedef void* FloxLiveEngineHandle;

  FLOX_EXPORT(group = "floxliveengine_disruptor")
  FloxLiveEngineHandle flox_live_engine_create(FloxRegistryHandle registry);
  FLOX_EXPORT(group = "floxliveengine_disruptor")
  void flox_live_engine_destroy(FloxLiveEngineHandle engine);

  // Attach a strategy to both TradeBus and BookUpdateBus.
  // on_signal is called from the consumer thread when the strategy emits an order.
  // The caller must ensure thread safety when submitting orders from on_signal.
  FLOX_EXPORT(group = "floxliveengine_disruptor")
  void flox_live_engine_add_strategy(FloxLiveEngineHandle engine,
                                     FloxStrategyHandle strategy,
                                     FloxOnSignalCallback on_signal,
                                     void* user_data);

  // Attach (or detach with rm = NULL) a risk manager to this engine.
  // The risk manager's `allow` callback fires on every signal before
  // it reaches the user-supplied on_signal callback. Safe to call before
  // or after start(); the engine takes a non-owning reference.
  FLOX_EXPORT(group = "risk")
  void flox_live_engine_set_risk_manager(FloxLiveEngineHandle engine,
                                         FloxRiskManagerHandle rm);

  // Attach (or detach with NULL) the global kill-switch and the per-order
  // validator. Evaluation order is KillSwitch → OrderValidator → RiskManager.
  FLOX_EXPORT(group = "risk")
  void flox_live_engine_set_kill_switch(FloxLiveEngineHandle engine,
                                        FloxKillSwitchHandle ks);
  FLOX_EXPORT(group = "risk")
  void flox_live_engine_set_order_validator(FloxLiveEngineHandle engine,
                                            FloxOrderValidatorHandle ov);

  // Post-emission observers. Fire after on_signal, in the order
  // PnLTracker → StorageSink. Detach with NULL.
  FLOX_EXPORT(group = "metrics")
  void flox_live_engine_set_pnl_tracker(FloxLiveEngineHandle engine,
                                        FloxPnLTrackerHandle tracker);
  FLOX_EXPORT(group = "storage")
  void flox_live_engine_set_storage_sink(FloxLiveEngineHandle engine,
                                         FloxStorageSinkHandle sink);

  // Attach (or detach with NULL) a market data recorder. Fires on every
  // published trade and book update; on_start / on_stop fire on engine
  // start/stop while attached.
  FLOX_EXPORT(group = "recorder")
  void flox_live_engine_set_market_data_recorder(FloxLiveEngineHandle engine,
                                                 FloxMarketDataRecorderHandle recorder);

  // Attach (or detach with NULL) a binding-supplied executor. When set,
  // emitted signals are forwarded to the executor (submit / cancel /
  // replace / cancel_all / submit_oco) instead of relying on the user's
  // on_signal callback to route the order to a broker. on_start /
  // on_stop fire balanced against engine start/stop.
  FLOX_EXPORT(group = "execution")
  void flox_live_engine_set_executor(FloxLiveEngineHandle engine,
                                     FloxExecutorHandle executor);

  FLOX_EXPORT(group = "floxliveengine_disruptor")
  void flox_live_engine_start(FloxLiveEngineHandle engine);
  FLOX_EXPORT(group = "floxliveengine_disruptor")
  void flox_live_engine_stop(FloxLiveEngineHandle engine);

  // Publish a trade tick to the TradeBus.
  // Lock-free. Returns immediately; consumer threads process asynchronously.
  FLOX_EXPORT(group = "floxliveengine_disruptor")
  void flox_live_engine_publish_trade(FloxLiveEngineHandle engine,
                                      uint32_t symbol,
                                      double price, double qty, uint8_t is_buy,
                                      int64_t exchange_ts_ns);

  // Publish a full L2 book snapshot to the BookUpdateBus.
  // Lock-free. Returns immediately; consumer threads process asynchronously.
  FLOX_EXPORT(group = "floxliveengine_disruptor")
  void flox_live_engine_publish_book_snapshot(FloxLiveEngineHandle engine,
                                              uint32_t symbol,
                                              const double* bid_prices,
                                              const double* bid_qtys,
                                              uint32_t n_bids,
                                              const double* ask_prices,
                                              const double* ask_qtys,
                                              uint32_t n_asks,
                                              int64_t exchange_ts_ns);

  // Publish a closed OHLC bar to the BarBus.
  // Lock-free. Returns immediately; consumer threads process asynchronously.
  FLOX_EXPORT(group = "floxliveengine_disruptor")
  void flox_live_engine_publish_bar(FloxLiveEngineHandle engine,
                                    uint32_t symbol,
                                    uint8_t bar_type, uint64_t bar_type_param,
                                    double open, double high, double low, double close,
                                    double volume, double buy_volume,
                                    int64_t start_time_ns, int64_t end_time_ns,
                                    uint8_t close_reason);

  // ============================================================
  // StrategyRunner — synchronous strategy host for live trading.
  //
  // Designed for Python, Codon, Node.js, and other language runtimes
  // that bring their own event loop and market data source (e.g. CCXT).
  //
  // Usage:
  //   1. Create a registry and register symbols.
  //   2. Create a runner with an on_signal callback.
  //   3. Create strategies and add them to the runner.
  //   4. Call flox_runner_start().
  //   5. Push market events (trades, book snapshots) as they arrive.
  //      Strategy callbacks fire synchronously before the call returns.
  //      When a strategy emits an order, on_signal is called immediately.
  //   6. Submit the received orders via your own execution backend.
  // ============================================================

  typedef void* FloxRunnerHandle;

  FLOX_EXPORT(group = "strategyrunner_synchronous")
  FloxRunnerHandle flox_runner_create(FloxRegistryHandle registry,
                                      FloxOnSignalCallback on_signal,
                                      void* user_data);
  FLOX_EXPORT(group = "strategyrunner_synchronous")
  void flox_runner_destroy(FloxRunnerHandle runner);

  // Attach a strategy (created via flox_strategy_create) to the runner.
  // The runner does NOT take ownership; call flox_strategy_destroy separately.
  FLOX_EXPORT(group = "strategyrunner_synchronous")
  void flox_runner_add_strategy(FloxRunnerHandle runner, FloxStrategyHandle strategy);

  // Attach (or detach with rm = NULL) a risk manager to this runner.
  // Same semantics as flox_live_engine_set_risk_manager — the `allow`
  // callback fires synchronously on every signal before on_signal does.
  FLOX_EXPORT(group = "risk")
  void flox_runner_set_risk_manager(FloxRunnerHandle runner,
                                    FloxRiskManagerHandle rm);

  // Same shape: attach/detach a kill-switch and an order validator.
  // Evaluation order on every signal: KillSwitch → OrderValidator → RiskManager.
  FLOX_EXPORT(group = "risk")
  void flox_runner_set_kill_switch(FloxRunnerHandle runner,
                                   FloxKillSwitchHandle ks);
  FLOX_EXPORT(group = "risk")
  void flox_runner_set_order_validator(FloxRunnerHandle runner,
                                       FloxOrderValidatorHandle ov);

  // Post-emission observers. Fire after on_signal in the order
  // PnLTracker → StorageSink.
  FLOX_EXPORT(group = "metrics")
  void flox_runner_set_pnl_tracker(FloxRunnerHandle runner,
                                   FloxPnLTrackerHandle tracker);
  FLOX_EXPORT(group = "storage")
  void flox_runner_set_storage_sink(FloxRunnerHandle runner,
                                    FloxStorageSinkHandle sink);

  // Attach (or detach with NULL) a market data recorder. on_trade and
  // on_book_update fire synchronously from flox_runner_on_trade /
  // flox_runner_on_book_snapshot. on_start / on_stop fire on runner
  // start/stop.
  FLOX_EXPORT(group = "recorder")
  void flox_runner_set_market_data_recorder(FloxRunnerHandle runner,
                                            FloxMarketDataRecorderHandle recorder);

  // Attach (or detach with NULL) a binding-supplied executor. When set,
  // emitted signals are forwarded to the executor (submit / cancel /
  // replace / cancel_all / submit_oco) in addition to the user's
  // on_signal callback. on_start / on_stop fire balanced against runner
  // start/stop.
  FLOX_EXPORT(group = "execution")
  void flox_runner_set_executor(FloxRunnerHandle runner,
                                FloxExecutorHandle executor);

  FLOX_EXPORT(group = "strategyrunner_synchronous")
  void flox_runner_start(FloxRunnerHandle runner);
  FLOX_EXPORT(group = "strategyrunner_synchronous")
  void flox_runner_stop(FloxRunnerHandle runner);

  // Push a trade tick. Strategy on_trade callbacks fire synchronously.
  FLOX_EXPORT(group = "strategyrunner_synchronous")
  void flox_runner_on_trade(FloxRunnerHandle runner, uint32_t symbol,
                            double price, double qty, uint8_t is_buy,
                            int64_t exchange_ts_ns);

  // Push a full L2 book snapshot. Strategy on_book callbacks fire synchronously.
  FLOX_EXPORT(group = "strategyrunner_synchronous")
  void flox_runner_on_book_snapshot(FloxRunnerHandle runner, uint32_t symbol,
                                    const double* bid_prices, const double* bid_qtys,
                                    uint32_t n_bids,
                                    const double* ask_prices, const double* ask_qtys,
                                    uint32_t n_asks,
                                    int64_t exchange_ts_ns);

  // Push a closed OHLC bar. Strategy on_bar callbacks fire synchronously.
  // bar_type / bar_type_param / close_reason match FloxBarData.
  FLOX_EXPORT(group = "strategyrunner_synchronous")
  void flox_runner_on_bar(FloxRunnerHandle runner, uint32_t symbol,
                          uint8_t bar_type, uint64_t bar_type_param,
                          double open, double high, double low, double close,
                          double volume, double buy_volume,
                          int64_t start_time_ns, int64_t end_time_ns,
                          uint8_t close_reason);

  // ============================================================
  // BacktestRunner — replay OHLCV data through a Strategy.
  //
  // Same Strategy class as StrategyRunner / LiveEngine; just a different
  // host. Emitted orders go to SimulatedExecutor; stats returned at end.
  //
  // Usage:
  //   1. Create registry, register symbols (flox_registry_add_symbol).
  //   2. Create a strategy (flox_strategy_create) with on_trade callbacks.
  //   3. flox_backtest_runner_create + flox_backtest_runner_set_strategy.
  //   4. flox_backtest_runner_run_csv  OR  flox_backtest_runner_run_ohlcv.
  //   5. Read FloxBacktestStats out parameter for results.
  //   6. flox_backtest_runner_destroy.
  // ============================================================

  typedef void* FloxBacktestRunnerHandle;

  FLOX_EXPORT(group = "backtestrunner_replay")
  FloxBacktestRunnerHandle flox_backtest_runner_create(FloxRegistryHandle registry,
                                                       double fee_rate,
                                                       double initial_capital);
  FLOX_EXPORT(group = "backtestrunner_replay")
  void flox_backtest_runner_destroy(FloxBacktestRunnerHandle runner);

  // Attach a strategy. BacktestRunner becomes the signal handler — emitted
  // orders are routed to SimulatedExecutor automatically.
  FLOX_EXPORT(group = "backtestrunner_replay")
  void flox_backtest_runner_set_strategy(FloxBacktestRunnerHandle runner,
                                         FloxStrategyHandle strategy);

  // Replay a CSV file (columns: timestamp, open, high, low, close, volume).
  // Returns 1 on success, 0 on error.
  FLOX_EXPORT(group = "backtestrunner_replay")
  int flox_backtest_runner_run_csv(FloxBacktestRunnerHandle runner,
                                   const char* path,
                                   const char* symbol,
                                   FloxBacktestStats* stats_out);

  // Drive the backtest off a `.floxlog` tape directory. Opens the
  // tape via `replay::createMultiSegmentReader` and dispatches every
  // event through the strategy (Strategy.on_trade / on_book_update
  // depending on what the tape contains).
  // Returns 1 on success, 0 on error.
  FLOX_EXPORT(group = "backtestrunner_replay")
  int flox_backtest_runner_run_tape(FloxBacktestRunnerHandle runner,
                                    const char* tape_dir,
                                    FloxBacktestStats* stats_out);

  // Drive the backtest off N `.floxlog` tapes merged on read.
  // Returns 1 on success, 0 on error.
  FLOX_EXPORT(group = "backtestrunner_replay")
  int flox_backtest_runner_run_tapes(FloxBacktestRunnerHandle runner,
                                     const char* const* tape_dirs,
                                     uint32_t n_dirs,
                                     FloxBacktestStats* stats_out);

  // Replay raw OHLCV arrays (timestamps in nanoseconds, close prices as double).
  // Each row produces one synthetic trade (price=close, qty=1). Strategy.on_trade fires.
  // Returns 1 on success, 0 on error.
  FLOX_EXPORT(group = "backtestrunner_replay")
  int flox_backtest_runner_run_ohlcv(FloxBacktestRunnerHandle runner,
                                     const int64_t* timestamps_ns,
                                     const double* close_prices,
                                     uint32_t n,
                                     const char* symbol,
                                     FloxBacktestStats* stats_out);

  // Replay full OHLCV bars (open/high/low/close/volume). Each row produces one
  // BarEvent. Strategy.on_bar fires; on_trade does NOT.
  // bar_type matches FloxBarData (0=Time, 1=Tick, …); bar_type_param is the
  // interval in ns / tick count / volume threshold.
  // Returns 1 on success, 0 on error.
  FLOX_EXPORT(group = "backtestrunner_replay")
  int flox_backtest_runner_run_bars(FloxBacktestRunnerHandle runner,
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
                                    FloxBacktestStats* stats_out);

  // Run a backtest pulling events from a binding-supplied replay source.
  // Fires source.on_start before reading, source.on_stop after the runner
  // has drained the stream. Returns 1 on success, 0 on error.
  FLOX_EXPORT(group = "backtestrunner_replay")
  int flox_backtest_runner_run_replay_source(FloxBacktestRunnerHandle runner,
                                             FloxReplaySourceHandle source,
                                             FloxBacktestStats* stats_out);

  // Returns a NEW BacktestResult handle that owns a copy of the runner's
  // last completed result, or NULL if no run has happened yet. Caller
  // takes ownership and must free with flox_backtest_result_destroy().
  // Stable across subsequent runs of the same runner — each run
  // overwrites the runner's internal copy, but already-taken handles
  // stay valid until destroyed.
  FLOX_EXPORT(group = "backtestrunner_replay")
  FloxBacktestResultHandle flox_backtest_runner_take_result(FloxBacktestRunnerHandle runner);

  // Attach a binding-side execution listener to the runner. Multiple
  // listeners may be attached; each one fires for every order event
  // emitted by the SimulatedExecutor. Caller retains ownership.
  FLOX_EXPORT(group = "execution")
  void flox_backtest_runner_add_execution_listener(FloxBacktestRunnerHandle runner,
                                                   FloxExecutionListenerHandle listener);

  // Attach an OrderJourneyTracer (an IOrderExecutionListener internally) to
  // the runner. The runner does not take ownership; the caller must keep
  // the tracer alive across the run.
  FLOX_EXPORT(group = "order_journey_tracer")
  void flox_backtest_runner_add_journey_tracer(FloxBacktestRunnerHandle runner,
                                               FloxOrderJourneyTracerHandle tracer);

  // Attach (or detach with NULL) a binding-supplied executor. When set,
  // emitted signals are routed to the executor instead of the built-in
  // SimulatedExecutor. The simulator stays in the runner so backtest
  // result accounting still works against simulator fills; if you want
  // the binding's executor to drive PnL accounting, surface fills via
  // a separate ExecutionListener path.
  FLOX_EXPORT(group = "execution")
  void flox_backtest_runner_set_executor(FloxBacktestRunnerHandle runner,
                                         FloxExecutorHandle executor);

  // Pre-trade gate parity with the live runner. All four hooks are
  // optional (NULL = no-op). The gate fires on entry-type signals
  // (Market / Limit / Stop* / TP* / TrailingStop); Cancel / CancelAll
  // / Modify pass through. Reduce-only orders bypass the gate so
  // tightening caps cannot strand a strategy in a position.
  FLOX_EXPORT(group = "backtestrunner_replay")
  void flox_backtest_runner_set_risk_manager(FloxBacktestRunnerHandle runner,
                                             FloxRiskManagerHandle rm);

  FLOX_EXPORT(group = "backtestrunner_replay")
  void flox_backtest_runner_set_kill_switch(FloxBacktestRunnerHandle runner,
                                            FloxKillSwitchHandle ks);

  FLOX_EXPORT(group = "backtestrunner_replay")
  void flox_backtest_runner_set_order_validator(FloxBacktestRunnerHandle runner,
                                                FloxOrderValidatorHandle ov);

  FLOX_EXPORT(group = "backtestrunner_replay")
  void flox_backtest_runner_set_pnl_tracker(FloxBacktestRunnerHandle runner,
                                            FloxPnLTrackerHandle tracker);

  // ============================================================
  // Walk-forward
  // ============================================================
  //
  // Anchored mode: train [0, t]; test [t, t + test_size]; t advances
  // by `step`. min_train_size sets the first split.
  // Sliding mode: train [t, t + train_size]; test [t + train_size,
  // t + train_size + test_size]; t advances by `step`.
  typedef struct
  {
    uint8_t mode;             // 0 = Anchored, 1 = Sliding
    uint64_t train_size;      // sliding mode only (bars)
    uint64_t test_size;       // bars per test window
    uint64_t step;            // 0 → defaults to test_size
    uint64_t min_train_size;  // anchored mode only (bars before first fold)
  } FloxWalkForwardConfig;

  typedef struct
  {
    uint64_t fold_index;
    uint64_t train_start_bar;
    uint64_t train_end_bar;
    uint64_t test_start_bar;
    uint64_t test_end_bar;
    int64_t train_start_ns;
    int64_t train_end_ns;
    int64_t test_start_ns;
    int64_t test_end_ns;
    FloxBacktestStats train_stats;
    FloxBacktestStats test_stats;
  } FloxWalkForwardFold;

  // Per-fold strategy factory. Called twice per fold (once for train,
  // once for test). Caller (binding) returns a fresh strategy handle
  // each call; the handle ownership stays with the caller — the
  // engine does not destroy it.
  typedef FloxStrategyHandle (*FloxWalkForwardFactoryFn)(
      void* user_data, uint64_t fold_index);

  // Run walk-forward over a CSV. Returns the total fold count.
  // If folds_out is NULL, computes the total without running anything;
  // pass max_folds = total in a second call to fill the buffer.
  FLOX_EXPORT(group = "walk_forward")
  uint32_t flox_walk_forward_run_csv(FloxRegistryHandle registry,
                                     const char* csv_path, const char* symbol,
                                     double fee_rate, double initial_capital,
                                     const FloxWalkForwardConfig* cfg,
                                     FloxWalkForwardFactoryFn factory,
                                     void* user_data,
                                     FloxWalkForwardFold* folds_out,
                                     uint32_t max_folds);

  // ============================================================
  // Grid search (sequential)
  // ============================================================
  //
  // Type-erased over vector<double> params. Each axis carries a list
  // of values; total combinations = product of axis lengths. Last axis
  // varies fastest (row-major flatten).
  typedef void* FloxGridSearchHandle;

  // Factory: caller fills out_stats from a backtest run on `params`.
  // Returns 1 if stats are valid, 0 to leave the slot zero-filled.
  typedef int (*FloxGridSearchFactoryFn)(
      void* user_data, uint64_t param_index,
      const double* params, uint32_t num_params,
      FloxBacktestStats* out_stats);

  FLOX_EXPORT(group = "grid_search")
  FloxGridSearchHandle flox_grid_search_create();

  FLOX_EXPORT(group = "grid_search")
  void flox_grid_search_destroy(FloxGridSearchHandle gs);

  FLOX_EXPORT(group = "grid_search")
  void flox_grid_search_add_axis(FloxGridSearchHandle gs,
                                 const double* values, uint32_t num_values);

  FLOX_EXPORT(group = "grid_search")
  uint64_t flox_grid_search_total(FloxGridSearchHandle gs);

  // Decode a flat index into (params_out[0..num_axes]). Returns the
  // number of axes (== params written), or 0 on bad index.
  FLOX_EXPORT(group = "grid_search")
  uint32_t flox_grid_search_params_for_index(FloxGridSearchHandle gs,
                                             uint64_t index,
                                             double* params_out,
                                             uint32_t max_params);

  // Run sequentially. Returns total combinations. If stats_out is NULL,
  // computes total without invoking the factory.
  FLOX_EXPORT(group = "grid_search")
  uint64_t flox_grid_search_run(FloxGridSearchHandle gs,
                                FloxGridSearchFactoryFn factory,
                                void* user_data,
                                FloxBacktestStats* stats_out,
                                uint32_t max_results);

  // ============================================================
  // Heatmap rendering
  // ============================================================
  //
  // z is row-major (length = rows * cols). row_labels / col_labels
  // are arrays of NUL-terminated strings; pass NULL (with the count
  // set to 0) to fall back to numeric indices.
  //
  // Two-call size-first pattern: pass NULL out_buf to get the byte
  // count, then allocate and call again. Returns the total size of
  // the rendered HTML in bytes (NOT including a NUL terminator).
  typedef struct
  {
    const double* z;
    uint32_t rows;
    uint32_t cols;
    const char* const* row_labels;
    uint32_t num_row_labels;
    const char* const* col_labels;
    uint32_t num_col_labels;
    const char* title;        // may be NULL
    const char* x_axis_name;  // may be NULL
    const char* y_axis_name;  // may be NULL
    const char* metric_name;  // may be NULL
  } FloxHeatmapData;

  FLOX_EXPORT(group = "heatmap")
  uint64_t flox_render_heatmap_html(const FloxHeatmapData* data,
                                    char* out_buf, uint64_t max_size);

  // ============================================================
  // Latency models (sampling primitive for backtest realism)
  // ============================================================
  //
  // Each model is created via a typed factory and queried through the
  // shared `flox_latency_*` accessors. Construction validates inputs
  // and returns NULL on failure (negative means/stddevs, empty
  // empirical inputs). All delays are returned as non-negative
  // nanoseconds.

  typedef void* FloxLatencyModelHandle;

  typedef struct
  {
    int64_t feed_ns;
    int64_t order_ns;
    int64_t fill_ns;
  } FloxLatencySample;

  FLOX_EXPORT(group = "latency_models")
  FloxLatencyModelHandle flox_latency_constant_create(int64_t feed_ns,
                                                      int64_t order_ns,
                                                      int64_t fill_ns);

  FLOX_EXPORT(group = "latency_models")
  FloxLatencyModelHandle flox_latency_gaussian_create(double feed_mean_ns,
                                                      double feed_stddev_ns,
                                                      double order_mean_ns,
                                                      double order_stddev_ns,
                                                      double fill_mean_ns,
                                                      double fill_stddev_ns,
                                                      uint64_t seed);

  FLOX_EXPORT(group = "latency_models")
  FloxLatencyModelHandle flox_latency_exponential_create(double feed_mean_ns,
                                                         double order_mean_ns,
                                                         double fill_mean_ns,
                                                         uint64_t seed);

  // Each samples array is copied; pass NULL with count 0 if a
  // component should always return 0.
  FLOX_EXPORT(group = "latency_models")
  FloxLatencyModelHandle flox_latency_empirical_create(const int64_t* feed_samples,
                                                       size_t feed_count,
                                                       const int64_t* order_samples,
                                                       size_t order_count,
                                                       const int64_t* fill_samples,
                                                       size_t fill_count,
                                                       uint64_t seed);

  FLOX_EXPORT(group = "latency_models")
  void flox_latency_destroy(FloxLatencyModelHandle model);

  FLOX_EXPORT(group = "latency_models")
  int64_t flox_latency_feed_delay(FloxLatencyModelHandle model);

  FLOX_EXPORT(group = "latency_models")
  int64_t flox_latency_order_delay(FloxLatencyModelHandle model);

  FLOX_EXPORT(group = "latency_models")
  int64_t flox_latency_fill_delay(FloxLatencyModelHandle model);

  FLOX_EXPORT(group = "latency_models")
  void flox_latency_sample(FloxLatencyModelHandle model, FloxLatencySample* out);

  FLOX_EXPORT(group = "latency_models")
  void flox_latency_reset(FloxLatencyModelHandle model, uint64_t seed);

  // ============================================================
  // Tape diff (replay-equivalence localization)
  // ============================================================
  //
  // Walks two .floxlog directories trade-by-trade and produces a
  // structured result the binding can render. The handle owns the
  // result vector; copy fields out via the accessors and free with
  // flox_tape_diff_destroy. NULL on read failure.

  typedef void* FloxTapeDiffHandle;

  typedef struct
  {
    int64_t exchange_ts_ns;
    int64_t price_raw;
    int64_t qty_raw;
    uint32_t symbol_id;
    uint8_t side;
  } FloxTapeDiffTrade;

  typedef struct
  {
    uint64_t index;
    FloxTapeDiffTrade left;
    FloxTapeDiffTrade right;
  } FloxTapeDiffMismatch;

  // max_mismatches=0 means "no cap"; field_tolerance_ns=0 means "exact".
  FLOX_EXPORT(group = "tape_diff")
  FloxTapeDiffHandle flox_tape_diff_create(const char* left_path,
                                           const char* right_path,
                                           uint32_t max_mismatches,
                                           int64_t field_tolerance_ns);

  FLOX_EXPORT(group = "tape_diff")
  void flox_tape_diff_destroy(FloxTapeDiffHandle handle);

  FLOX_EXPORT(group = "tape_diff")
  uint64_t flox_tape_diff_left_count(FloxTapeDiffHandle handle);

  FLOX_EXPORT(group = "tape_diff")
  uint64_t flox_tape_diff_right_count(FloxTapeDiffHandle handle);

  // Returns 1 and writes *out_index on present, 0 if there is no
  // divergence (tapes are equal).
  FLOX_EXPORT(group = "tape_diff")
  uint8_t flox_tape_diff_first_divergence(FloxTapeDiffHandle handle,
                                          uint64_t* out_index);

  FLOX_EXPORT(group = "tape_diff")
  uint8_t flox_tape_diff_equal(FloxTapeDiffHandle handle);

  FLOX_EXPORT(group = "tape_diff")
  uint64_t flox_tape_diff_mismatch_count(FloxTapeDiffHandle handle);

  // Two-call size pattern: pass NULL out to learn the count, then
  // allocate and pass a buffer big enough for at least
  // flox_tape_diff_mismatch_count entries. Writes
  // min(count, max_entries) entries and returns the number written.
  FLOX_EXPORT(group = "tape_diff")
  uint64_t flox_tape_diff_copy_mismatches(FloxTapeDiffHandle handle,
                                          FloxTapeDiffMismatch* out,
                                          uint64_t max_entries);

  // ============================================================
  // Portfolio risk aggregator
  // ============================================================
  //
  // Cross-strategy daily PnL, gross / net exposure, kill switch on
  // drawdown / loss / gross / concentration limits. Single-process,
  // mutex-guarded; one handle per portfolio.

  typedef void* FloxPortfolioRiskHandle;

  // Field-mask bits for flox_portfolio_risk_update.
  // Set ALL (0x3F) to overwrite every field; combine bits to update
  // only specific dimensions (e.g. gross+net only).
  // bit 0: realized_pnl, 1: unrealized_pnl, 2: fees,
  // bit 3: gross_exposure, 4: net_exposure, 5: trade_count.

  typedef struct
  {
    uint8_t has_max_drawdown_pct;
    double max_drawdown_pct;
    uint8_t has_max_daily_loss;
    double max_daily_loss;
    uint8_t has_max_gross_exposure;
    double max_gross_exposure;
    uint8_t has_max_concentration_pct;
    double max_concentration_pct;
  } FloxPortfolioRiskRules;

  typedef struct
  {
    double realized_pnl;
    double unrealized_pnl;
    double fees;
    double gross_exposure;
    double net_exposure;
    uint64_t trade_count;
  } FloxStrategyAccountFields;

  // Single breach record. `rule` and `detail` strings are owned by
  // the aggregator and remain valid until the next state-mutating
  // call on the same handle; copy them out if the caller needs to
  // outlive that.
  typedef struct
  {
    const char* rule;
    double value;
    double limit;
    const char* detail;
  } FloxBreach;

  FLOX_EXPORT(group = "portfolio_risk")
  FloxPortfolioRiskHandle flox_portfolio_risk_create(
      const FloxPortfolioRiskRules* rules, double initial_equity);

  FLOX_EXPORT(group = "portfolio_risk")
  void flox_portfolio_risk_destroy(FloxPortfolioRiskHandle handle);

  FLOX_EXPORT(group = "portfolio_risk")
  void flox_portfolio_risk_update(FloxPortfolioRiskHandle handle,
                                  const char* name,
                                  const FloxStrategyAccountFields* fields,
                                  uint8_t field_mask);

  FLOX_EXPORT(group = "portfolio_risk")
  void flox_portfolio_risk_remove(FloxPortfolioRiskHandle handle, const char* name);

  FLOX_EXPORT(group = "portfolio_risk")
  void flox_portfolio_risk_reset_kill_switch(FloxPortfolioRiskHandle handle);

  // Returns 1 and writes *out_breach if the order is rejected.
  // Returns 0 and leaves out_breach untouched if the order is allowed.
  FLOX_EXPORT(group = "portfolio_risk")
  uint8_t flox_portfolio_risk_check_order(FloxPortfolioRiskHandle handle,
                                          const char* strategy,
                                          double notional,
                                          const char* side,
                                          FloxBreach* out_breach);

  // Headline snapshot fields. The aggregator owns the strings
  // referenced via flox_portfolio_risk_breach_at; they remain valid
  // until the next mutating call.
  FLOX_EXPORT(group = "portfolio_risk")
  double flox_portfolio_risk_total_daily_pnl(FloxPortfolioRiskHandle handle);

  FLOX_EXPORT(group = "portfolio_risk")
  double flox_portfolio_risk_total_gross_exposure(FloxPortfolioRiskHandle handle);

  FLOX_EXPORT(group = "portfolio_risk")
  double flox_portfolio_risk_current_equity(FloxPortfolioRiskHandle handle);

  FLOX_EXPORT(group = "portfolio_risk")
  double flox_portfolio_risk_drawdown_pct(FloxPortfolioRiskHandle handle);

  FLOX_EXPORT(group = "portfolio_risk")
  uint8_t flox_portfolio_risk_kill_switch_active(FloxPortfolioRiskHandle handle);

  FLOX_EXPORT(group = "portfolio_risk")
  uint64_t flox_portfolio_risk_breach_count(FloxPortfolioRiskHandle handle);

  // Returns 1 and writes *out on success, 0 if index is out of range.
  // The handle owns the string memory; it is invalidated by the next
  // mutating call.
  FLOX_EXPORT(group = "portfolio_risk")
  uint8_t flox_portfolio_risk_breach_at(FloxPortfolioRiskHandle handle,
                                        uint64_t index,
                                        FloxBreach* out);

  FLOX_EXPORT(group = "portfolio_risk")
  uint64_t flox_portfolio_risk_account_count(FloxPortfolioRiskHandle handle);

  // ============================================================
  // Execution algorithms (TWAP / VWAP / Iceberg / POV)
  // ============================================================
  //
  // Each algo is a state machine. The caller drives it with `step`
  // and reads any newly emitted child orders via the `pending_*`
  // accessors, then dispatches them to its own executor and calls
  // `clear_pending` before the next step. Iceberg gates the next
  // slice on `report_fill`. POV uses `observe_volume` to track
  // market activity.

  typedef void* FloxExecAlgoHandle;

  typedef struct
  {
    uint64_t order_id;
    int64_t timestamp_ns;
    double qty;
    double price;
    uint8_t type;  // 0 = market, 1 = limit
  } FloxExecChildOrder;

  // side: 0 = buy, 1 = sell. type: 0 = market, 1 = limit.
  // limit_price is ignored for market orders.

  FLOX_EXPORT(group = "execution_algos")
  FloxExecAlgoHandle flox_exec_twap_create(double target_qty, uint8_t side,
                                           uint32_t symbol, uint8_t type,
                                           double limit_price,
                                           int64_t duration_ns,
                                           uint32_t slice_count,
                                           int64_t start_time_ns);

  // volume_curve_ts and volume_curve_vol must be parallel arrays of
  // length n. ts in nanoseconds, vol must be non-negative; total
  // volume across the curve must be positive.
  FLOX_EXPORT(group = "execution_algos")
  FloxExecAlgoHandle flox_exec_vwap_create(double target_qty, uint8_t side,
                                           uint32_t symbol, uint8_t type,
                                           double limit_price,
                                           const int64_t* volume_curve_ts,
                                           const double* volume_curve_vol,
                                           size_t n);

  FLOX_EXPORT(group = "execution_algos")
  FloxExecAlgoHandle flox_exec_iceberg_create(double target_qty, uint8_t side,
                                              uint32_t symbol, uint8_t type,
                                              double limit_price,
                                              double visible_qty);

  FLOX_EXPORT(group = "execution_algos")
  FloxExecAlgoHandle flox_exec_pov_create(double target_qty, uint8_t side,
                                          uint32_t symbol, uint8_t type,
                                          double limit_price,
                                          double participation_rate,
                                          double min_slice_qty);

  FLOX_EXPORT(group = "execution_algos")
  void flox_exec_destroy(FloxExecAlgoHandle handle);

  FLOX_EXPORT(group = "execution_algos")
  void flox_exec_step(FloxExecAlgoHandle handle, int64_t now_ns);

  FLOX_EXPORT(group = "execution_algos")
  void flox_exec_report_fill(FloxExecAlgoHandle handle, double qty);

  FLOX_EXPORT(group = "execution_algos")
  void flox_exec_observe_volume(FloxExecAlgoHandle handle, double qty);

  FLOX_EXPORT(group = "execution_algos")
  size_t flox_exec_pending_count(FloxExecAlgoHandle handle);

  FLOX_EXPORT(group = "execution_algos")
  uint8_t flox_exec_pending_at(FloxExecAlgoHandle handle, size_t index,
                               FloxExecChildOrder* out);

  FLOX_EXPORT(group = "execution_algos")
  void flox_exec_clear_pending(FloxExecAlgoHandle handle);

  FLOX_EXPORT(group = "execution_algos")
  double flox_exec_target_qty(FloxExecAlgoHandle handle);

  FLOX_EXPORT(group = "execution_algos")
  double flox_exec_submitted_qty(FloxExecAlgoHandle handle);

  FLOX_EXPORT(group = "execution_algos")
  double flox_exec_filled_qty(FloxExecAlgoHandle handle);

  FLOX_EXPORT(group = "execution_algos")
  double flox_exec_remaining_qty(FloxExecAlgoHandle handle);

  FLOX_EXPORT(group = "execution_algos")
  uint8_t flox_exec_is_done(FloxExecAlgoHandle handle);

  // ============================================================
  // Delta book compression (tape format)
  // ============================================================
  //
  // Encodes a stream of L2 snapshots into anchor snapshots plus
  // deltas. The on-disk format already supports both event types
  // through BookRecordHeader.type (0 = snapshot, 1 = delta). This
  // surface is the state-keeping layer that decides what to emit
  // per call.
  //
  // Convention in a delta payload: a level with qty_raw == 0 means
  // "remove this price level"; a level with qty_raw > 0 means "set
  // this price level to this quantity".

  typedef void* FloxDeltaBookEncoderHandle;
  typedef void* FloxDeltaBookReplayerHandle;

  // anchor_every controls cadence: every N events the encoder emits
  // a full snapshot regardless of diff size, so a reader can seek
  // and replay forward. 0 means "always anchor" (snapshot-only,
  // i.e. the existing writer behaviour).
  FLOX_EXPORT(group = "delta_book")
  FloxDeltaBookEncoderHandle flox_delta_book_encoder_create(uint32_t anchor_every);

  FLOX_EXPORT(group = "delta_book")
  void flox_delta_book_encoder_destroy(FloxDeltaBookEncoderHandle handle);

  FLOX_EXPORT(group = "delta_book")
  void flox_delta_book_encoder_reset(FloxDeltaBookEncoderHandle handle, uint32_t symbol_id);

  FLOX_EXPORT(group = "delta_book")
  void flox_delta_book_encoder_reset_all(FloxDeltaBookEncoderHandle handle);

  // Feed a full snapshot. After the call:
  //   *out_is_delta -> 0 if the encoder emits an anchor snapshot,
  //                    1 if it emits a delta.
  //   *out_bid_count / *out_ask_count -> number of BookLevel entries
  //   the caller can then pull via flox_delta_book_encoder_copy_*.
  // Buffers are owned by the encoder and remain valid until the next
  // encode() on the same symbol.
  FLOX_EXPORT(group = "delta_book")
  void flox_delta_book_encoder_encode(FloxDeltaBookEncoderHandle handle,
                                      uint32_t symbol_id,
                                      const FloxBookLevel* bids, size_t bid_count,
                                      const FloxBookLevel* asks, size_t ask_count,
                                      uint8_t* out_is_delta,
                                      uint64_t* out_bid_count,
                                      uint64_t* out_ask_count);

  // Copy the most recent encode() output into caller-allocated
  // buffers. Returns the number of entries written, capped at
  // max_entries.
  FLOX_EXPORT(group = "delta_book")
  uint64_t flox_delta_book_encoder_copy_bids(FloxDeltaBookEncoderHandle handle,
                                             FloxBookLevel* out, uint64_t max_entries);

  FLOX_EXPORT(group = "delta_book")
  uint64_t flox_delta_book_encoder_copy_asks(FloxDeltaBookEncoderHandle handle,
                                             FloxBookLevel* out, uint64_t max_entries);

  // Replayer: takes the events back and reconstructs full
  // snapshots per symbol.
  FLOX_EXPORT(group = "delta_book")
  FloxDeltaBookReplayerHandle flox_delta_book_replayer_create(void);

  FLOX_EXPORT(group = "delta_book")
  void flox_delta_book_replayer_destroy(FloxDeltaBookReplayerHandle handle);

  FLOX_EXPORT(group = "delta_book")
  void flox_delta_book_replayer_reset(FloxDeltaBookReplayerHandle handle, uint32_t symbol_id);

  // Apply one event (type=0 snapshot, type=1 delta) and write the
  // resulting per-side counts into *out_bid_count / *out_ask_count.
  // Pull the levels via copy_bids / copy_asks.
  FLOX_EXPORT(group = "delta_book")
  void flox_delta_book_replayer_apply(FloxDeltaBookReplayerHandle handle,
                                      uint8_t type, uint32_t symbol_id,
                                      const FloxBookLevel* bids, size_t bid_count,
                                      const FloxBookLevel* asks, size_t ask_count,
                                      uint64_t* out_bid_count,
                                      uint64_t* out_ask_count);

  FLOX_EXPORT(group = "delta_book")
  uint64_t flox_delta_book_replayer_copy_bids(FloxDeltaBookReplayerHandle handle,
                                              FloxBookLevel* out, uint64_t max_entries);

  FLOX_EXPORT(group = "delta_book")
  uint64_t flox_delta_book_replayer_copy_asks(FloxDeltaBookReplayerHandle handle,
                                              FloxBookLevel* out, uint64_t max_entries);

  // ============================================================
  // Strategy run trace (.floxrun)
  // ============================================================
  //
  // A `.floxrun` directory holds the events a strategy emitted during
  // a run: signals (decisions), order events (submit / cancel /
  // ack / reject), and fills. It sits alongside the `.floxlog`
  // tape(s) the run consumed and complements them — tape carries
  // exchange-side market data, this carries strategy-side trace.
  //
  // The recorder writes per-kind segment files (signals-NNN.bin,
  // orders-NNN.bin, fills-NNN.bin) plus manifest.json. The reader
  // opens an existing directory and exposes one read pass per
  // record kind. Variable-length payloads (signal name / payload,
  // order reject reason) come back via the standard two-call size
  // pattern: call once with NULL buffer to learn the size, then
  // again with a sized buffer to pull the bytes.

  typedef void* FloxRunRecorderHandle;
  typedef void* FloxRunReaderHandle;

  // Recorder. Opens (or creates) a directory at `path`. The
  // strategy_id, strategy_hash, and run_started_ns flow into
  // manifest.json on close.
  FLOX_EXPORT(group = "floxrun")
  FloxRunRecorderHandle flox_run_recorder_create(const char* path,
                                                 const char* strategy_id,
                                                 const char* strategy_hash,
                                                 int64_t run_started_ns);

  FLOX_EXPORT(group = "floxrun")
  void flox_run_recorder_destroy(FloxRunRecorderHandle handle);

  // Append one tape reference to the manifest. Call before close().
  FLOX_EXPORT(group = "floxrun")
  void flox_run_recorder_add_tape_ref(FloxRunRecorderHandle handle,
                                      const char* path,
                                      const char* content_hash,
                                      int64_t first_event_ns,
                                      int64_t last_event_ns);

  FLOX_EXPORT(group = "floxrun")
  void flox_run_recorder_set_run_ended_ns(FloxRunRecorderHandle handle, int64_t ns);

  FLOX_EXPORT(group = "floxrun")
  void flox_run_recorder_write_signal(FloxRunRecorderHandle handle,
                                      int64_t run_ts_ns, int64_t feed_ts_ns,
                                      uint32_t signal_id, uint32_t flags,
                                      int64_t strength_raw,
                                      const char* name, size_t name_len,
                                      const uint32_t* symbol_ids, size_t symbol_count,
                                      const uint8_t* payload, size_t payload_len);

  FLOX_EXPORT(group = "floxrun")
  void flox_run_recorder_write_order_event(FloxRunRecorderHandle handle,
                                           int64_t run_ts_ns, int64_t feed_ts_ns,
                                           uint64_t order_id, uint64_t parent_signal_id,
                                           int64_t price_raw, int64_t qty_raw,
                                           uint32_t symbol_id, uint8_t event_kind,
                                           uint8_t side, uint8_t order_type,
                                           uint32_t flags,
                                           const char* reason, size_t reason_len);

  FLOX_EXPORT(group = "floxrun")
  void flox_run_recorder_write_fill(FloxRunRecorderHandle handle,
                                    int64_t run_ts_ns, int64_t feed_ts_ns,
                                    uint64_t order_id, uint64_t fill_id,
                                    int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                                    uint32_t symbol_id, uint8_t side, uint8_t liquidity);

  // Flush + finalize all open segments + manifest.json. Idempotent.
  FLOX_EXPORT(group = "floxrun")
  void flox_run_recorder_close(FloxRunRecorderHandle handle);

  // ============================================================
  // Trace recorder auto-attach
  // ============================================================
  //
  // Attach a `FloxRunRecorderHandle` to a `FloxRunnerHandle` so every
  // signal the strategy emits gets captured into the .floxrun trace
  // without per-strategy instrumentation. Pass `recorder = NULL` to
  // detach. Order / fill auto-capture is a follow-up; today the
  // recorder captures signals only — order events still need to be
  // written by the user's executor / on_fill callback.

  FLOX_EXPORT(group = "trace_attach")
  void flox_runner_attach_trace_recorder(FloxRunnerHandle runner,
                                         FloxRunRecorderHandle recorder);
  FLOX_EXPORT(group = "trace_attach")
  void flox_runner_set_trace_feed_ts_ns(FloxRunnerHandle runner, int64_t feed_ts_ns);

  // Mirror an order event into the attached recorder. No-op when no
  // recorder is attached. Wire from the user's executor wrapper —
  // call this after the wrapper's `on_submitted` / `on_canceled` /
  // `on_rejected` / etc. The runner stamps `run_ts_ns` with the
  // current wall-clock and uses the most recent `feed_ts_ns` from
  // `flox_runner_set_trace_feed_ts_ns`.
  // event_kind matches OrderEventKind in run_format_v1.h:
  //   1=Submit, 2=Cancel, 3=Modify, 4=Ack, 5=Reject, 6=Expire.
  FLOX_EXPORT(group = "trace_attach")
  void flox_runner_trace_order_event(FloxRunnerHandle runner, uint64_t order_id,
                                     uint64_t parent_signal_id, uint32_t symbol_id,
                                     uint8_t event_kind, uint8_t side, uint8_t order_type,
                                     int64_t price_raw, int64_t qty_raw, uint32_t flags);

  // Mirror a fill into the attached recorder. liquidity: 0=Unknown,
  // 1=Maker, 2=Taker.
  FLOX_EXPORT(group = "trace_attach")
  void flox_runner_trace_fill(FloxRunnerHandle runner, uint64_t order_id, uint64_t fill_id,
                              int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                              uint32_t symbol_id, uint8_t side, uint8_t liquidity);

  // Reader. Opens an existing `.floxrun` directory and parses
  // manifest.json.
  FLOX_EXPORT(group = "floxrun")
  FloxRunReaderHandle flox_run_reader_open(const char* path);

  FLOX_EXPORT(group = "floxrun")
  void flox_run_reader_close(FloxRunReaderHandle handle);

  // Manifest accessors (read-only views into the parsed manifest).
  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_strategy_id(FloxRunReaderHandle handle, char* out, uint64_t max_bytes);

  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_strategy_hash(FloxRunReaderHandle handle, char* out, uint64_t max_bytes);

  FLOX_EXPORT(group = "floxrun")
  int64_t flox_run_reader_run_started_ns(FloxRunReaderHandle handle);

  FLOX_EXPORT(group = "floxrun")
  int64_t flox_run_reader_run_ended_ns(FloxRunReaderHandle handle);

  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_tape_ref_count(FloxRunReaderHandle handle);

  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_tape_ref_path(FloxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes);

  // Counts. Returns 0 if the kind was never written.
  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_signal_count(FloxRunReaderHandle handle);

  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_order_event_count(FloxRunReaderHandle handle);

  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_fill_count(FloxRunReaderHandle handle);

  // Per-record accessors. Index in [0, count). Variable-length
  // strings/payloads use the two-call size pattern: pass NULL/0 to
  // learn the size, then call again with a sized buffer.
  FLOX_EXPORT(group = "floxrun")
  void flox_run_reader_signal_header(FloxRunReaderHandle handle, uint64_t index,
                                     int64_t* out_run_ts, int64_t* out_feed_ts,
                                     uint32_t* out_signal_id, uint32_t* out_flags,
                                     int64_t* out_strength_raw,
                                     uint64_t* out_name_len, uint64_t* out_symbol_count,
                                     uint64_t* out_payload_len);

  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_signal_name(FloxRunReaderHandle handle, uint64_t index,
                                       char* out, uint64_t max_bytes);

  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_signal_symbol_ids(FloxRunReaderHandle handle, uint64_t index,
                                             uint32_t* out, uint64_t max_entries);

  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_signal_payload(FloxRunReaderHandle handle, uint64_t index,
                                          uint8_t* out, uint64_t max_bytes);

  FLOX_EXPORT(group = "floxrun")
  void flox_run_reader_order_event_header(FloxRunReaderHandle handle, uint64_t index,
                                          int64_t* out_run_ts, int64_t* out_feed_ts,
                                          uint64_t* out_order_id, uint64_t* out_parent_signal_id,
                                          int64_t* out_price_raw, int64_t* out_qty_raw,
                                          uint32_t* out_symbol_id, uint8_t* out_event_kind,
                                          uint8_t* out_side, uint8_t* out_order_type,
                                          uint32_t* out_flags, uint64_t* out_reason_len);

  FLOX_EXPORT(group = "floxrun")
  uint64_t flox_run_reader_order_event_reason(FloxRunReaderHandle handle, uint64_t index,
                                              char* out, uint64_t max_bytes);

  FLOX_EXPORT(group = "floxrun")
  void flox_run_reader_fill(FloxRunReaderHandle handle, uint64_t index,
                            int64_t* out_run_ts, int64_t* out_feed_ts,
                            uint64_t* out_order_id, uint64_t* out_fill_id,
                            int64_t* out_price_raw, int64_t* out_qty_raw, int64_t* out_fee_raw,
                            uint32_t* out_symbol_id, uint8_t* out_side, uint8_t* out_liquidity);

  // Bar-close dispatch recorder.
  //
  // Cross-binding parity test fixture for the documented bar-close
  // ordering rule: on tied closes, MultiTimeframeAggregator dispatches
  // bars in the order their timeframes were registered. The recorder
  // wraps a BarBus + aggregator + recording subscriber so a binding
  // only needs to register timeframes, push trades, and read back the
  // dispatch sequence.

  typedef void* FloxBarDispatchRecorderHandle;

  FLOX_EXPORT(group = "bar_dispatch")
  FloxBarDispatchRecorderHandle flox_bar_dispatch_recorder_create(void);
  FLOX_EXPORT(group = "bar_dispatch")
  void flox_bar_dispatch_recorder_destroy(FloxBarDispatchRecorderHandle h);

  // Returns slot index of the registered timeframe (or kMaxTimeframes on full).
  FLOX_EXPORT(group = "bar_dispatch")
  uint32_t flox_bar_dispatch_recorder_add_time_seconds(FloxBarDispatchRecorderHandle h,
                                                       uint32_t seconds);

  FLOX_EXPORT(group = "bar_dispatch")
  void flox_bar_dispatch_recorder_on_trade(FloxBarDispatchRecorderHandle h,
                                           uint32_t symbol, double price, double qty,
                                           int64_t ts_ns);

  // Drains the bus so all bars at the final tied close fire. Must be
  // called before reading count / param_at / type_at.
  FLOX_EXPORT(group = "bar_dispatch")
  void flox_bar_dispatch_recorder_finalize(FloxBarDispatchRecorderHandle h);

  FLOX_EXPORT(group = "bar_dispatch")
  uint32_t flox_bar_dispatch_recorder_count(FloxBarDispatchRecorderHandle h);
  FLOX_EXPORT(group = "bar_dispatch")
  uint8_t flox_bar_dispatch_recorder_type_at(FloxBarDispatchRecorderHandle h, uint32_t index);
  FLOX_EXPORT(group = "bar_dispatch")
  uint64_t flox_bar_dispatch_recorder_param_at(FloxBarDispatchRecorderHandle h, uint32_t index);

  // ============================================================
  // Streaming tape aggregators (W14-T019)
  //
  // Single-pass dispatch over a captured `.floxlog` via a panel of
  // streaming aggregators. Five concrete aggregators (event-type
  // stats / time-bucketed count / volume-bin / top-N peak windows /
  // window-count quantiles) implement a common contract; the reader
  // walks the tape once and forwards every event to every attached
  // aggregator's `onEvent`, then calls `finalize` on each.
  //
  // Lifecycle: create an aggregator handle, pass an array of them to
  // `flox_data_reader_run` (or the merged equivalent), read the
  // result via the type-specific `read_result` family, destroy the
  // handle when done. All result accessors are call-twice: first to
  // get the row count, second with a caller-allocated buffer.
  // ============================================================

  typedef void* FloxAggregatorHandle;

  // Event-type filter applied inside each aggregator's onEvent.
  // Matches `flox::replay::AggregatorEventFilter`: 1 = Trades only,
  // 2 = Books only, 3 = Both.
  typedef enum
  {
    FLOX_AGG_FILTER_TRADES = 1,
    FLOX_AGG_FILTER_BOOKS_ONLY = 2,
    FLOX_AGG_FILTER_BOTH = 3,
  } FloxAggregatorEventFilter;

  // Side encoding shared across BinCount / VolumeBin result rows:
  // 0 = aggregate (no split), 1 = BUY, 2 = SELL.
  //
  // Constructors take `symbol_filter` as `(uint32_t*, uint32_t count)`
  // — pass NULL/0 to disable filtering (all symbols admitted).

  FLOX_EXPORT(group = "tape_aggregator")
  FloxAggregatorHandle flox_event_type_stats_aggregator_create(
      FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  FLOX_EXPORT(group = "tape_aggregator")
  FloxAggregatorHandle flox_bin_count_aggregator_create(
      int64_t bucket_ns, uint8_t by_side, uint8_t by_symbol,
      FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  FLOX_EXPORT(group = "tape_aggregator")
  FloxAggregatorHandle flox_volume_bin_aggregator_create(
      int64_t bucket_ns, uint8_t by_side, uint8_t by_symbol,
      FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  // OHLCBinAggregator: per-bucket open/high/low/close of trade
  // price_raw. Trade-only; by_symbol controls per-symbol split (no
  // by_side because OHLC by side is not a generally useful primitive).
  FLOX_EXPORT(group = "tape_aggregator")
  FloxAggregatorHandle flox_ohlc_bin_aggregator_create(
      int64_t bucket_ns, uint8_t by_symbol,
      FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  // PeakAggregator: `oversample_factor` 0 means "use the engine default"
  // (currently 100). Pass non-zero to override.
  FLOX_EXPORT(group = "tape_aggregator")
  FloxAggregatorHandle flox_peak_aggregator_create(
      const int64_t* window_ns_list, uint32_t window_count, uint32_t top_n,
      uint32_t oversample_factor, FloxAggregatorEventFilter event_filter,
      const uint32_t* symbol_filter, uint32_t symbol_filter_count);

  FLOX_EXPORT(group = "tape_aggregator")
  FloxAggregatorHandle flox_quantile_aggregator_create(
      const int64_t* window_ns_list, uint32_t window_count,
      const double* quantiles, uint32_t quantile_count,
      FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
      uint32_t symbol_filter_count);

  // Universal destroy — works on any handle returned by the create
  // functions above. Safe to call with NULL.
  FLOX_EXPORT(group = "tape_aggregator")
  void flox_aggregator_destroy(FloxAggregatorHandle h);

  // Single-pass dispatch. `aggregators` is an array of handles (any
  // mix of types); returns non-zero on success, zero on failure
  // (typically: empty data directory or read error). An empty array
  // is a no-op and returns success without decompressing anything.
  // n_threads=1 (or 0 → treated as 1) preserves single-threaded
  // semantics. n_threads>1 partitions the segment list across worker
  // threads; each worker clones the panel via cloneEmpty(), and the
  // reader merges worker panels into the caller's originals before
  // finalize. Effective worker count is clamped to segment count.
  FLOX_EXPORT(group = "tape_aggregator")
  uint8_t flox_data_reader_run(FloxDataReaderHandle reader,
                               FloxAggregatorHandle* aggregators,
                               uint32_t aggregator_count, uint32_t n_threads);

  // Progress callback for `flox_data_reader_run` (single-threaded
  // path only — n_threads=1). The callback fires at most once per
  // `interval_ms` from inside the run loop. `pct` is the fraction of
  // total events dispatched, in [0.0, 1.0]; `cursor_ts_ns` is the
  // exchange timestamp of the most recent dispatched event. The
  // callback returns non-zero to keep running, zero to request
  // cancellation — on cancel, the run finalizes aggregators with
  // whatever state has been accumulated and `flox_data_reader_run`
  // returns 0. `interval_ms = 0` is treated as 1000ms.
  typedef uint8_t (*FloxProgressCallback)(void* user_data, double pct,
                                          int64_t cursor_ts_ns);

  FLOX_EXPORT(group = "data_reader")
  void flox_data_reader_set_progress_callback(
      FloxDataReaderHandle reader, FloxProgressCallback cb,
      void* user_data, uint32_t interval_ms);

  // Clear any callback previously installed via
  // `flox_data_reader_set_progress_callback`. Equivalent to passing
  // NULL as the callback function pointer.
  FLOX_EXPORT(group = "data_reader")
  void flox_data_reader_clear_progress_callback(FloxDataReaderHandle reader);

  // MergedTapeReader::run is single-threaded for the moment — symbol
  // rekey is per-instance and a per-worker MergedTapeReader would not
  // share global symbol ids. The n_threads parameter is reserved for
  // future use; values > 1 are accepted but currently ignored.
  FLOX_EXPORT(group = "tape_aggregator")
  uint8_t flox_merged_tape_reader_run(FloxMergedTapeReaderHandle reader,
                                      FloxAggregatorHandle* aggregators,
                                      uint32_t aggregator_count,
                                      uint32_t n_threads);

  // Result row layouts. Mirror the C++ Row structs byte-for-byte;
  // bindings memcpy from the C ABI buffer into their native form.

  typedef struct
  {
    uint32_t symbol_id;
    uint64_t trades;
    uint64_t book_snapshots;
    uint64_t book_deltas;
  } FloxEventTypeStatsRow;

  typedef struct
  {
    int64_t bucket_ts_ns;
    uint32_t symbol_id;
    uint8_t side;
    uint64_t count;
  } FloxBinCountRow;

  typedef struct
  {
    int64_t bucket_ts_ns;
    uint32_t symbol_id;
    uint8_t side;
    int64_t qty_raw;
  } FloxVolumeBinRow;

  typedef struct
  {
    int64_t bucket_ts_ns;
    uint32_t symbol_id;
    int64_t open_raw;
    int64_t high_raw;
    int64_t low_raw;
    int64_t close_raw;
  } FloxOHLCBinRow;

  typedef struct
  {
    int64_t window_ns;
    uint64_t count;
    int64_t start_ns;
  } FloxPeakRow;

  typedef struct
  {
    int64_t window_ns;
    double quantile;
    uint64_t count;
  } FloxQuantileRow;

  // Result accessors. Each is two-call: first form returns the row
  // count (pass `rows_out=NULL`, `max_rows=0`); second form fills the
  // caller's buffer (returns the number of rows actually written,
  // capped at `max_rows`). The shape returned by each aggregator is
  // documented at its create-function site.
  FLOX_EXPORT(group = "tape_aggregator")
  uint32_t flox_event_type_stats_read_result(FloxAggregatorHandle h,
                                             FloxEventTypeStatsRow* rows_out,
                                             uint32_t max_rows);

  FLOX_EXPORT(group = "tape_aggregator")
  uint32_t flox_bin_count_read_result(FloxAggregatorHandle h,
                                      FloxBinCountRow* rows_out,
                                      uint32_t max_rows);

  FLOX_EXPORT(group = "tape_aggregator")
  uint32_t flox_volume_bin_read_result(FloxAggregatorHandle h,
                                       FloxVolumeBinRow* rows_out,
                                       uint32_t max_rows);

  FLOX_EXPORT(group = "tape_aggregator")
  uint32_t flox_ohlc_bin_read_result(FloxAggregatorHandle h,
                                     FloxOHLCBinRow* rows_out,
                                     uint32_t max_rows);

  FLOX_EXPORT(group = "tape_aggregator")
  uint32_t flox_peak_read_result(FloxAggregatorHandle h,
                                 FloxPeakRow* rows_out, uint32_t max_rows);

  FLOX_EXPORT(group = "tape_aggregator")
  uint32_t flox_quantile_read_result(FloxAggregatorHandle h,
                                     FloxQuantileRow* rows_out,
                                     uint32_t max_rows);

  // ---- Tiered fee schedule ---------------------------------------
  //
  // Volume-tiered maker/taker fee ladder. Add tiers + record fills;
  // the schedule maintains a 30-day rolling notional and resolves
  // the active tier on lookup.

  typedef void* FloxFeeScheduleHandle;

  FLOX_EXPORT(group = "fee_schedule")
  FloxFeeScheduleHandle flox_fee_schedule_create(void);
  FLOX_EXPORT(group = "fee_schedule")
  void flox_fee_schedule_destroy(FloxFeeScheduleHandle h);

  FLOX_EXPORT(group = "fee_schedule")
  void flox_fee_schedule_add_tier(FloxFeeScheduleHandle h, double min_notional_30d,
                                  double maker_bps, double taker_bps);

  // Canned profile: "binance_um_futures" | "bybit_linear" | "okx_swap" |
  // "deribit". Unknown names are a no-op.
  FLOX_EXPORT(group = "fee_schedule")
  void flox_fee_schedule_load_profile(FloxFeeScheduleHandle h,
                                      const char* profile_name);

  FLOX_EXPORT(group = "fee_schedule")
  void flox_fee_schedule_record_fill(FloxFeeScheduleHandle h, int64_t ts_ns,
                                     double notional);

  FLOX_EXPORT(group = "fee_schedule")
  double flox_fee_schedule_fee_for(FloxFeeScheduleHandle h, int64_t ts_ns,
                                   double notional, uint8_t is_maker);

  FLOX_EXPORT(group = "fee_schedule")
  uint32_t flox_fee_schedule_current_tier(FloxFeeScheduleHandle h);

  FLOX_EXPORT(group = "fee_schedule")
  double flox_fee_schedule_rolling_notional(FloxFeeScheduleHandle h);

  FLOX_EXPORT(group = "fee_schedule")
  uint32_t flox_fee_schedule_tier_transitions(FloxFeeScheduleHandle h,
                                              int64_t* out_buf, uint32_t max_events);

  FLOX_EXPORT(group = "fee_schedule")
  void flox_fee_schedule_reset_rolling(FloxFeeScheduleHandle h);

  // ---- Funding schedule ------------------------------------------
  //
  // Models perpetual-futures funding payments. Build via constant
  // interval + rate or via a recorded tape, then call `tick` to walk
  // funding events between the last tick and now.

  typedef void* FloxFundingScheduleHandle;

  FLOX_EXPORT(group = "funding_schedule")
  FloxFundingScheduleHandle flox_funding_schedule_create(void);
  FLOX_EXPORT(group = "funding_schedule")
  void flox_funding_schedule_destroy(FloxFundingScheduleHandle h);

  // Configure as constant-interval with the given fixed rate.
  FLOX_EXPORT(group = "funding_schedule")
  void flox_funding_schedule_set_constant(FloxFundingScheduleHandle h, int64_t interval_ns,
                                          double rate);

  // Configure as a tape: timestamps_ns and rates arrays must have
  // length n_events.
  FLOX_EXPORT(group = "funding_schedule")
  void flox_funding_schedule_set_tape(FloxFundingScheduleHandle h,
                                      const int64_t* timestamps_ns, const double* rates,
                                      uint32_t n_events);

  // Configure as a per-symbol tape. Each of the three arrays must
  // hold `n_entries` elements: timestamps_ns[i], symbols[i], rates[i]
  // describe row i. symbol == 0 acts as a wildcard (every symbol gets
  // this rate at that timestamp).
  FLOX_EXPORT(group = "funding_schedule")
  void flox_funding_schedule_set_tape_by_symbol(FloxFundingScheduleHandle h,
                                                const int64_t* timestamps_ns,
                                                const uint32_t* symbols,
                                                const double* rates,
                                                uint32_t n_entries);

  // Read a CSV with columns timestamp_ns, symbol, funding_rate. Lines
  // starting with '#' and a leading alphabetic header row are
  // skipped. Returns 1 on success, 0 if the file could not be opened
  // (existing tape state preserved on failure).
  FLOX_EXPORT(group = "funding_schedule")
  uint8_t flox_funding_schedule_load_tape(FloxFundingScheduleHandle h,
                                          const char* path);

  // Load a canned profile: "binance_um_futures" | "bybit_linear" |
  // "okx_swap" | "bitget_hourly". Rate defaults to zero; override
  // with set_constant_rate or use a tape.
  FLOX_EXPORT(group = "funding_schedule")
  void flox_funding_schedule_load_profile(FloxFundingScheduleHandle h,
                                          const char* profile_name);

  FLOX_EXPORT(group = "funding_schedule")
  void flox_funding_schedule_set_constant_rate(FloxFundingScheduleHandle h, double rate);

  FLOX_EXPORT(group = "funding_schedule")
  void flox_funding_schedule_reset(FloxFundingScheduleHandle h);

  // Walk funding events in (last_tick, now_ns]. The caller supplies
  // per-symbol positions (signed, long positive) and mark prices.
  // Output is laid out as 6 doubles per event in `out_buf`:
  //   [0] timestamp_ns (as f64 — cast to int64 to recover)
  //   [1] symbol_id    (as f64)
  //   [2] rate
  //   [3] mark_price
  //   [4] position_signed
  //   [5] amount
  // First call with out_buf=NULL returns the event count.
  FLOX_EXPORT(group = "funding_schedule")
  uint32_t flox_funding_schedule_tick(FloxFundingScheduleHandle h, int64_t now_ns,
                                      const uint32_t* symbols, const double* positions,
                                      const double* mark_prices, uint32_t n_symbols,
                                      double* out_buf, uint32_t max_events);

  // ---- Live queue position estimator -----------------------------
  //
  // Client-side estimator: feed it order placements, trades, book
  // updates and (our) fills, read back estimated queue-ahead per
  // resting order. The estimator wraps OrderQueueTracker; see
  // include/flox/execution/live_queue_position_estimator.h for the
  // heuristic and its limits.

  typedef void* FloxLiveQueuePositionHandle;

  FLOX_EXPORT(group = "live_queue_position")
  FloxLiveQueuePositionHandle flox_live_queue_position_create(void);
  FLOX_EXPORT(group = "live_queue_position")
  void flox_live_queue_position_destroy(FloxLiveQueuePositionHandle h);

  // halfLifeNs: confidence decays as `exp(-elapsed * ln(2) / halfLife)`.
  // Zero disables time decay. Default ≈ 60s.
  FLOX_EXPORT(group = "live_queue_position")
  void flox_live_queue_position_set_confidence_half_life_ns(
      FloxLiveQueuePositionHandle h, int64_t half_life_ns);

  // Per-shrink confidence multiplier (proportional-shrink heuristic).
  // Default 0.85.
  FLOX_EXPORT(group = "live_queue_position")
  void flox_live_queue_position_set_shrink_factor(FloxLiveQueuePositionHandle h,
                                                  double factor);

  // side: 0 = BUY, 1 = SELL.
  FLOX_EXPORT(group = "live_queue_position")
  void flox_live_queue_position_on_order_placed(FloxLiveQueuePositionHandle h,
                                                uint32_t symbol, uint8_t side,
                                                int64_t price_raw, uint64_t order_id,
                                                int64_t order_qty_raw,
                                                int64_t level_qty_raw, int64_t ts_ns);

  FLOX_EXPORT(group = "live_queue_position")
  void flox_live_queue_position_on_order_cancelled(FloxLiveQueuePositionHandle h,
                                                   uint64_t order_id, int64_t ts_ns);

  FLOX_EXPORT(group = "live_queue_position")
  void flox_live_queue_position_on_order_filled(FloxLiveQueuePositionHandle h,
                                                uint64_t order_id,
                                                int64_t cumulative_fill_raw,
                                                int64_t ts_ns);

  FLOX_EXPORT(group = "live_queue_position")
  void flox_live_queue_position_on_trade(FloxLiveQueuePositionHandle h, uint32_t symbol,
                                         int64_t price_raw, int64_t qty_raw,
                                         int64_t ts_ns);

  // Trade with the venue's per-trade `is_hidden` flag. Behaviour
  // depends on the configured hidden-order policy.
  FLOX_EXPORT(group = "live_queue_position")
  void flox_live_queue_position_on_trade_with_flag(FloxLiveQueuePositionHandle h,
                                                   uint32_t symbol, int64_t price_raw,
                                                   int64_t qty_raw, int64_t ts_ns,
                                                   uint8_t is_hidden);

  // Hidden-order attribution policy.
  // 0 = Ignore (default), 1 = TrustTradeFlag, 2 = InferIfTradeExceedsVisible.
  FLOX_EXPORT(group = "live_queue_position")
  void flox_live_queue_position_set_hidden_order_policy(FloxLiveQueuePositionHandle h,
                                                        uint8_t policy);

  FLOX_EXPORT(group = "live_queue_position")
  void flox_live_queue_position_on_level_update(FloxLiveQueuePositionHandle h,
                                                uint32_t symbol, uint8_t side,
                                                int64_t price_raw, int64_t new_qty_raw,
                                                int64_t ts_ns);

  // Snapshot out is laid out as 5 i64 + 1 f64 slots in a single int64
  // buffer (the f64 is bit-cast). The caller provides storage for 6
  // slots:
  //   [0] order_id
  //   [1] queue_ahead_raw
  //   [2] total_raw
  //   [3] last_update_ns
  //   [4] confidence as f64 bit-cast (use memcpy to read)
  //   [5] hidden_volume_seen_raw
  // Returns 1 if the order is tracked, 0 otherwise.
  FLOX_EXPORT(group = "live_queue_position")
  uint8_t flox_live_queue_position_snapshot(FloxLiveQueuePositionHandle h,
                                            uint64_t order_id, int64_t now_ns,
                                            int64_t* out_slots);

  FLOX_EXPORT(group = "live_queue_position")
  uint32_t flox_live_queue_position_tracked_count(FloxLiveQueuePositionHandle h);

  // ============================================================
  // Liquidation engine + insurance fund + ADL
  // ============================================================
  typedef void* FloxLiquidationEngineHandle;

  FLOX_EXPORT(group = "liquidation_engine")
  FloxLiquidationEngineHandle flox_liquidation_engine_create(void);
  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_destroy(FloxLiquidationEngineHandle h);

  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_add_tier(FloxLiquidationEngineHandle h,
                                        double min_notional, double mm_fraction);

  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_set_insurance_fund_capital(FloxLiquidationEngineHandle h,
                                                          double capital);
  FLOX_EXPORT(group = "liquidation_engine")
  double flox_liquidation_engine_insurance_fund_balance(FloxLiquidationEngineHandle h);
  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_set_adl_enabled(FloxLiquidationEngineHandle h, uint8_t enabled);
  // ADL ranking strategy: 0=pnl_ratio, 1=binance, 2=bybit, 3=position_size.
  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_set_adl_ranking(FloxLiquidationEngineHandle h, uint8_t ranking);
  FLOX_EXPORT(group = "liquidation_engine")
  uint8_t flox_liquidation_engine_adl_ranking(FloxLiquidationEngineHandle h);
  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_set_liquidation_slippage_bps(FloxLiquidationEngineHandle h,
                                                            double bps);

  // Side is encoded in signed `quantity` (+long, -short).
  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_open_position(FloxLiquidationEngineHandle h,
                                             uint64_t account_id, uint32_t symbol,
                                             double quantity, double entry_price,
                                             double equity);
  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_close_position(FloxLiquidationEngineHandle h,
                                              uint64_t account_id, uint32_t symbol);

  // Apply a mark; returns liquidations on this tick.
  FLOX_EXPORT(group = "liquidation_engine")
  uint32_t flox_liquidation_engine_on_mark(FloxLiquidationEngineHandle h,
                                           uint32_t symbol, double mark_price);

  FLOX_EXPORT(group = "liquidation_engine")
  uint64_t flox_liquidation_engine_liquidations_count(FloxLiquidationEngineHandle h);
  FLOX_EXPORT(group = "liquidation_engine")
  uint64_t flox_liquidation_engine_insurance_payments_count(FloxLiquidationEngineHandle h);
  FLOX_EXPORT(group = "liquidation_engine")
  uint64_t flox_liquidation_engine_adl_closeouts_count(FloxLiquidationEngineHandle h);

  // Canned profiles: 0=binance_um_futures, 1=bybit_linear, 2=okx_swap.
  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_load_profile(FloxLiquidationEngineHandle h, uint8_t profile);

  // Attach a SimulatedExecutor so liquidation orders route through
  // it as market orders. Pass NULL to detach.
  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_set_executor(FloxLiquidationEngineHandle h,
                                            FloxSimulatedExecutorHandle executor);

  // T039: distribution-level cascade statistics. Each *_size returns
  // the vector size; *_copy copies up to `max` entries to caller's
  // buffer and returns the number copied. Use *_size first to size
  // the allocation.
  FLOX_EXPORT(group = "liquidation_engine")
  uint32_t flox_liquidation_engine_deficits_paid_by_fund_size(FloxLiquidationEngineHandle h);
  FLOX_EXPORT(group = "liquidation_engine")
  uint32_t flox_liquidation_engine_deficits_paid_by_fund_copy(FloxLiquidationEngineHandle h,
                                                              double* out, uint32_t max);
  FLOX_EXPORT(group = "liquidation_engine")
  uint32_t flox_liquidation_engine_deficits_paid_by_adl_size(FloxLiquidationEngineHandle h);
  FLOX_EXPORT(group = "liquidation_engine")
  uint32_t flox_liquidation_engine_deficits_paid_by_adl_copy(FloxLiquidationEngineHandle h,
                                                             double* out, uint32_t max);
  FLOX_EXPORT(group = "liquidation_engine")
  uint32_t flox_liquidation_engine_cascade_sizes_size(FloxLiquidationEngineHandle h);
  FLOX_EXPORT(group = "liquidation_engine")
  uint32_t flox_liquidation_engine_cascade_sizes_copy(FloxLiquidationEngineHandle h,
                                                      uint32_t* out, uint32_t max);
  FLOX_EXPORT(group = "liquidation_engine")
  uint32_t flox_liquidation_engine_fund_balance_history_size(FloxLiquidationEngineHandle h);
  FLOX_EXPORT(group = "liquidation_engine")
  uint32_t flox_liquidation_engine_fund_balance_history_copy(FloxLiquidationEngineHandle h,
                                                             double* out, uint32_t max);

  // Ordinal of the onMark call that fired the first ADL closeout.
  // UINT64_MAX if no ADL has fired.
  FLOX_EXPORT(group = "liquidation_engine")
  uint64_t flox_liquidation_engine_ticks_to_first_adl(FloxLiquidationEngineHandle h);

  FLOX_EXPORT(group = "liquidation_engine")
  void flox_liquidation_engine_reset_stats(FloxLiquidationEngineHandle h);

#ifdef __cplusplus
}
#endif
