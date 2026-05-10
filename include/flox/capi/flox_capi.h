/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

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

  // Status codes match flox::OrderEventStatus. Kept as a plain int so
  // callers don't have to reach into the C++ enum.
  //   0 NEW                 1 SUBMITTED       2 ACCEPTED
  //   3 PARTIALLY_FILLED    4 FILLED          5 PENDING_CANCEL
  //   6 CANCELED            7 EXPIRED         8 REJECTED
  //   9 REPLACED           10 PENDING_TRIGGER 11 TRIGGERED
  //  12 TRAILING_UPDATED
  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol_id;
    uint8_t side;        // 0 BUY, 1 SELL
    uint8_t order_type;  // 0 LIMIT, 1 MARKET, 2 STOP_MARKET, ...
    uint8_t status;      // FloxOrderEventStatus, see comment above
    uint8_t _pad;
    int64_t fill_qty_raw;
    int64_t fill_price_raw;
    int64_t exchange_ts_ns;
    const char* reject_reason;  // null when status != REJECTED
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

  typedef struct
  {
    FloxOnTradeCallback on_trade;
    FloxOnBookCallback on_book;
    FloxOnBarCallback on_bar;
    FloxOnStartCallback on_start;
    FloxOnStopCallback on_stop;
    // Order-event hooks. Default-initialised to nullptr by C `{}`-init,
    // so existing callers that don't set them get no-op behaviour. The
    // runner only invokes a callback when its function pointer is
    // non-null.
    FloxOnFillCallback on_fill;
    FloxOnOrderUpdateCallback on_order_update;
    void* user_data;
  } FloxStrategyCallbacks;

  // ============================================================
  // Symbol registry
  // ============================================================

  FloxRegistryHandle flox_registry_create(void);
  void flox_registry_destroy(FloxRegistryHandle registry);
  uint32_t flox_registry_add_symbol(FloxRegistryHandle registry, const char* exchange,
                                    const char* name, double tick_size);

  // Symbol name resolution
  uint8_t flox_registry_get_symbol_id(FloxRegistryHandle registry, const char* exchange,
                                      const char* name, uint32_t* id_out);
  uint8_t flox_registry_get_symbol_name(FloxRegistryHandle registry, uint32_t symbol_id,
                                        char* exchange_out, size_t exchange_len, char* name_out,
                                        size_t name_len);
  uint32_t flox_registry_symbol_count(FloxRegistryHandle registry);

  // ============================================================
  // Strategy lifecycle
  // ============================================================

  FloxStrategyHandle flox_strategy_create(uint32_t id, const uint32_t* symbols,
                                          uint32_t num_symbols, FloxRegistryHandle registry,
                                          FloxStrategyCallbacks callbacks);
  FloxStrategyHandle flox_strategy_create_p(uint32_t id, const uint32_t* symbols,
                                            uint32_t num_symbols, FloxRegistryHandle registry,
                                            const FloxStrategyCallbacks* callbacks);
  void flox_strategy_destroy(FloxStrategyHandle strategy);

  // Atomically replace the strategy's callback set without dropping
  // any subscriptions, in-flight orders, or open connections. The
  // next dispatched event sees the new callbacks. on_stop fires on
  // the old user_data before the swap; on_start fires on the new
  // user_data after.
  void flox_strategy_replace_callbacks(FloxStrategyHandle strategy,
                                       FloxStrategyCallbacks callbacks);
  void flox_strategy_replace_callbacks_p(FloxStrategyHandle strategy,
                                         const FloxStrategyCallbacks* callbacks);

  // ============================================================
  // Signal emission (returns OrderId, 0 on failure)
  // ============================================================

  uint64_t flox_emit_market_buy(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw);
  uint64_t flox_emit_market_sell(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw);
  uint64_t flox_emit_limit_buy(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                               int64_t qty_raw);
  uint64_t flox_emit_limit_sell(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                int64_t qty_raw);
  void flox_emit_cancel(FloxStrategyHandle s, uint64_t order_id);
  void flox_emit_cancel_all(FloxStrategyHandle s, uint32_t symbol);
  void flox_emit_modify(FloxStrategyHandle s, uint64_t order_id, int64_t new_price_raw,
                        int64_t new_qty_raw);
  uint64_t flox_emit_stop_market(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                 int64_t trigger_raw, int64_t qty_raw);
  uint64_t flox_emit_stop_limit(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw);
  uint64_t flox_emit_take_profit_market(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                        int64_t trigger_raw, int64_t qty_raw);
  uint64_t flox_emit_trailing_stop(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                   int64_t offset_raw, int64_t qty_raw);
  uint64_t flox_emit_trailing_stop_percent(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                           int32_t callback_bps, int64_t qty_raw);
  uint64_t flox_emit_take_profit_limit(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                       int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw);
  uint64_t flox_emit_limit_buy_tif(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                   int64_t qty_raw, uint8_t time_in_force);
  uint64_t flox_emit_limit_sell_tif(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                    int64_t qty_raw, uint8_t time_in_force);
  uint64_t flox_emit_close_position(FloxStrategyHandle s, uint32_t symbol);

  // ============================================================
  // Context queries
  // ============================================================

  int64_t flox_position_raw(FloxStrategyHandle s, uint32_t symbol);
  int64_t flox_last_trade_price_raw(FloxStrategyHandle s, uint32_t symbol);
  int64_t flox_best_bid_raw(FloxStrategyHandle s, uint32_t symbol);
  int64_t flox_best_ask_raw(FloxStrategyHandle s, uint32_t symbol);
  int64_t flox_mid_price_raw(FloxStrategyHandle s, uint32_t symbol);
  void flox_get_symbol_context(FloxStrategyHandle s, uint32_t symbol, FloxSymbolContext* out);
  int32_t flox_get_order_status(FloxStrategyHandle s, uint64_t order_id);

  // ============================================================
  // Fixed-point conversion helpers
  // ============================================================

  int64_t flox_price_from_double(double value);
  double flox_price_to_double(int64_t raw);
  int64_t flox_quantity_from_double(double value);
  double flox_quantity_to_double(int64_t raw);

  // ============================================================
  // Indicator functions (stateless, array-in/array-out)
  // ============================================================

  void flox_indicator_ema(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_sma(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_rsi(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_atr(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);
  void flox_indicator_macd(const double* input, size_t len, size_t fast_period,
                           size_t slow_period, size_t signal_period, double* macd_out,
                           double* signal_out, double* hist_out);
  void flox_indicator_bollinger(const double* input, size_t len, size_t period, double multiplier,
                                double* upper, double* middle, double* lower);

  // Moving average variants
  void flox_indicator_rma(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_dema(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_tema(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_kama(const double* input, size_t len, size_t period, size_t fast, size_t slow,
                           double* output);

  // Trend
  void flox_indicator_slope(const double* input, size_t len, size_t length, double* output);
  void flox_indicator_adx(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* adx_out, double* plus_di_out,
                          double* minus_di_out);

  // Oscillators
  void flox_indicator_cci(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);
  void flox_indicator_stochastic(const double* high, const double* low, const double* close,
                                 size_t len, size_t k_period, size_t d_period, double* k_out,
                                 double* d_out);
  void flox_indicator_chop(const double* high, const double* low, const double* close, size_t len,
                           size_t period, double* output);

  // Volume
  void flox_indicator_obv(const double* close, const double* volume, size_t len, double* output);
  void flox_indicator_vwap(const double* close, const double* volume, size_t len, size_t window,
                           double* output);
  void flox_indicator_cvd(const double* open, const double* high, const double* low,
                          const double* close, const double* volume, size_t len, double* output);

  // Statistical
  void flox_indicator_skewness(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_kurtosis(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_parkinson_vol(const double* high, const double* low, size_t len,
                                    size_t period, double* output);
  void flox_indicator_rogers_satchell_vol(const double* open, const double* high, const double* low,
                                          const double* close, size_t len, size_t period,
                                          double* output);
  void flox_indicator_rolling_zscore(const double* input, size_t len, size_t period,
                                     double* output);
  void flox_indicator_shannon_entropy(const double* input, size_t len, size_t period, size_t bins,
                                      double* output);
  void flox_indicator_correlation(const double* x, const double* y, size_t len, size_t period,
                                  double* output);

  // Augmented Dickey-Fuller test. `regression` accepts: "n", "c", "ct".
  // Writes the t-statistic, approximate p-value, and AIC-selected lag.
  void flox_indicator_adf(const double* input, size_t len, size_t max_lag, const char* regression,
                          double* test_stat_out, double* p_value_out, size_t* used_lag_out);
  // AutoCorrelation: Pearson correlation between x[t] and x[t-lag] over a
  // rolling window. First valid index is (window + lag - 1).
  void flox_indicator_autocorrelation(const double* input, size_t len, size_t window, size_t lag,
                                      double* output);

  // ============================================================
  // Targets (forward-looking labels, batch only)
  //
  // Targets read into the future relative to t. They are intentionally
  // separate from indicators: feeding them into a live update loop is a
  // look-ahead-bias bug.
  // ============================================================

  void flox_target_future_return(const double* close, size_t len, size_t horizon, double* output);
  void flox_target_future_ctc_volatility(const double* close, size_t len, size_t horizon,
                                         double* output);
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

  FloxIndicatorGraphHandle flox_indicator_graph_create(void);
  void flox_indicator_graph_destroy(FloxIndicatorGraphHandle g);

  // Pass NULL for high/low/volume to default high=low=close and volume=0.
  void flox_indicator_graph_set_bars(FloxIndicatorGraphHandle g, uint32_t symbol,
                                     const double* close, const double* high, const double* low,
                                     const double* volume, size_t len);

  // deps: array of `num_deps` C-string node names (or NULL if num_deps == 0).
  void flox_indicator_graph_add_node(FloxIndicatorGraphHandle g, const char* name,
                                     const char* const* deps, size_t num_deps,
                                     FloxGraphNodeFn fn, void* user_data);

  // Returns a pointer to the cached output for (symbol, name) and writes its
  // length to *len_out. The pointer is owned by the graph and is valid until
  // the next invalidate / destroy call. Returns NULL on error (unknown node,
  // cycle, etc.).
  const double* flox_indicator_graph_require(FloxIndicatorGraphHandle g, uint32_t symbol,
                                             const char* name, size_t* len_out);

  // Same as require but only returns a pointer if the node has already been
  // computed; never triggers compute. Returns NULL if not yet computed.
  const double* flox_indicator_graph_get(FloxIndicatorGraphHandle g, uint32_t symbol,
                                         const char* name, size_t* len_out);

  // Field accessors return cached double arrays for the symbol's bars.
  const double* flox_indicator_graph_close(FloxIndicatorGraphHandle g, uint32_t symbol,
                                           size_t* len_out);
  const double* flox_indicator_graph_high(FloxIndicatorGraphHandle g, uint32_t symbol,
                                          size_t* len_out);
  const double* flox_indicator_graph_low(FloxIndicatorGraphHandle g, uint32_t symbol,
                                         size_t* len_out);
  const double* flox_indicator_graph_volume(FloxIndicatorGraphHandle g, uint32_t symbol,
                                            size_t* len_out);

  void flox_indicator_graph_invalidate(FloxIndicatorGraphHandle g, uint32_t symbol);
  void flox_indicator_graph_invalidate_all(FloxIndicatorGraphHandle g);

  // ── Streaming path on the same handle ────────────────────────────
  // Same graph object, both APIs. step() appends one bar; node cache is
  // invalidated; current() returns the latest value of a named node.

  void flox_indicator_graph_step(FloxIndicatorGraphHandle g, uint32_t symbol, double open,
                                 double high, double low, double close, double volume);
  double flox_indicator_graph_current(FloxIndicatorGraphHandle g, uint32_t symbol,
                                      const char* name);
  uint32_t flox_indicator_graph_bar_count(FloxIndicatorGraphHandle g, uint32_t symbol);
  void flox_indicator_graph_reset(FloxIndicatorGraphHandle g, uint32_t symbol);
  void flox_indicator_graph_reset_all(FloxIndicatorGraphHandle g);

  // ── Deprecated streaming-graph shim ──────────────────────────────
  // The old separate StreamingIndicatorGraph type has been collapsed
  // into IndicatorGraph. These names are now thin forwarders to the new
  // ones and will be removed in a future major version. New code: use
  // flox_indicator_graph_* (one handle, both compute and step).

  typedef FloxIndicatorGraphHandle FloxStreamingGraphHandle;

  FloxStreamingGraphHandle flox_streaming_graph_create(void);
  void flox_streaming_graph_destroy(FloxStreamingGraphHandle sg);
  void flox_streaming_graph_add_node(FloxStreamingGraphHandle sg, const char* name,
                                     const char* const* deps, size_t num_deps,
                                     FloxGraphNodeFn fn, void* user_data);
  void flox_streaming_graph_step(FloxStreamingGraphHandle sg, uint32_t symbol, double open,
                                 double high, double low, double close, double volume);
  double flox_streaming_graph_current(FloxStreamingGraphHandle sg, uint32_t symbol,
                                      const char* name);
  uint32_t flox_streaming_graph_bar_count(FloxStreamingGraphHandle sg, uint32_t symbol);
  void flox_streaming_graph_reset(FloxStreamingGraphHandle sg, uint32_t symbol);
  void flox_streaming_graph_reset_all(FloxStreamingGraphHandle sg);
  const double* flox_streaming_graph_close(FloxStreamingGraphHandle sg, uint32_t symbol,
                                           size_t* len_out);
  const double* flox_streaming_graph_high(FloxStreamingGraphHandle sg, uint32_t symbol,
                                          size_t* len_out);
  const double* flox_streaming_graph_low(FloxStreamingGraphHandle sg, uint32_t symbol,
                                         size_t* len_out);
  const double* flox_streaming_graph_volume(FloxStreamingGraphHandle sg, uint32_t symbol,
                                            size_t* len_out);

  // ============================================================
  // Order book
  // ============================================================

  FloxBookHandle flox_book_create(double tick_size);
  void flox_book_destroy(FloxBookHandle book);
  void flox_book_apply_snapshot(FloxBookHandle book, const double* bid_prices,
                                const double* bid_qtys, size_t bid_len, const double* ask_prices,
                                const double* ask_qtys, size_t ask_len);
  void flox_book_apply_delta(FloxBookHandle book, const double* bid_prices, const double* bid_qtys,
                             size_t bid_len, const double* ask_prices, const double* ask_qtys,
                             size_t ask_len);
  uint8_t flox_book_best_bid(FloxBookHandle book, double* price_out);
  uint8_t flox_book_best_ask(FloxBookHandle book, double* price_out);
  uint8_t flox_book_mid(FloxBookHandle book, double* price_out);
  uint8_t flox_book_spread(FloxBookHandle book, double* spread_out);
  double flox_book_bid_at_price(FloxBookHandle book, double price);
  double flox_book_ask_at_price(FloxBookHandle book, double price);
  uint8_t flox_book_is_crossed(FloxBookHandle book);
  void flox_book_clear(FloxBookHandle book);

  // ============================================================
  // Simulated executor (backtesting)
  // ============================================================

  FloxSimulatedExecutorHandle flox_simulated_executor_create(void);
  void flox_simulated_executor_destroy(FloxSimulatedExecutorHandle executor);
  void flox_simulated_executor_submit_order(FloxSimulatedExecutorHandle executor, uint64_t id, uint8_t side,
                                            double price, double quantity, uint8_t order_type,
                                            uint32_t symbol);
  void flox_simulated_executor_cancel_order(FloxSimulatedExecutorHandle executor, uint64_t order_id);
  void flox_simulated_executor_cancel_all(FloxSimulatedExecutorHandle executor, uint32_t symbol);
  void flox_simulated_executor_on_bar(FloxSimulatedExecutorHandle executor, uint32_t symbol, double close_price);
  void flox_simulated_executor_on_trade(FloxSimulatedExecutorHandle executor, uint32_t symbol, double price,
                                        uint8_t is_buy);
  void flox_simulated_executor_advance_clock(FloxSimulatedExecutorHandle executor, int64_t timestamp_ns);
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

  uint32_t flox_aggregate_time_bars(const int64_t* timestamps, const double* prices,
                                    const double* quantities, const uint8_t* is_buy, size_t len,
                                    double interval_seconds, FloxBar* bars_out,
                                    uint32_t max_bars);
  uint32_t flox_aggregate_tick_bars(const int64_t* timestamps, const double* prices,
                                    const double* quantities, const uint8_t* is_buy, size_t len,
                                    uint32_t tick_count, FloxBar* bars_out, uint32_t max_bars);
  uint32_t flox_aggregate_volume_bars(const int64_t* timestamps, const double* prices,
                                      const double* quantities, const uint8_t* is_buy, size_t len,
                                      double volume_threshold, FloxBar* bars_out,
                                      uint32_t max_bars);

  // ============================================================
  // Multi-timeframe alignment helpers
  // ============================================================

  uint8_t flox_strategy_last_closed_bar(FloxStrategyHandle s, uint32_t symbol,
                                        uint8_t bar_type, uint64_t param,
                                        FloxBar* out);
  uint32_t flox_strategy_last_n_closed_bars(FloxStrategyHandle s, uint32_t symbol,
                                            uint8_t bar_type, uint64_t param,
                                            FloxBar* bars_out, uint32_t max_bars);
  uint32_t flox_strategy_get_bar_ring_capacity(FloxStrategyHandle s);
  void flox_strategy_set_bar_ring_capacity(FloxStrategyHandle s, uint32_t capacity);

  // ============================================================
  // Multi-leg order group
  // ============================================================

  typedef void* FloxOrderGroupHandle;

  FloxOrderGroupHandle flox_order_group_create(uint64_t parent_signal_id, uint8_t policy);
  void flox_order_group_destroy(FloxOrderGroupHandle h);

  uint32_t flox_order_group_add_market_leg(FloxOrderGroupHandle h, uint32_t symbol,
                                           uint8_t side, int64_t qty_raw);
  uint32_t flox_order_group_add_limit_leg(FloxOrderGroupHandle h, uint32_t symbol,
                                          uint8_t side, int64_t price_raw,
                                          int64_t qty_raw);

  uint32_t flox_order_group_leg_count(FloxOrderGroupHandle h);
  uint8_t flox_order_group_leg_state(FloxOrderGroupHandle h, uint32_t leg_index);
  int64_t flox_order_group_leg_filled_raw(FloxOrderGroupHandle h, uint32_t leg_index);
  uint64_t flox_order_group_leg_order_id(FloxOrderGroupHandle h, uint32_t leg_index);

  void flox_order_group_record_submit(FloxOrderGroupHandle h, uint32_t leg_index,
                                      uint64_t order_id);
  void flox_order_group_record_fill(FloxOrderGroupHandle h, uint32_t leg_index,
                                    int64_t cumulative_qty_raw);
  void flox_order_group_record_cancel(FloxOrderGroupHandle h, uint32_t leg_index);
  void flox_order_group_record_failure(FloxOrderGroupHandle h, uint32_t leg_index);

  uint8_t flox_order_group_state(FloxOrderGroupHandle h);
  uint32_t flox_order_group_recommended_actions(FloxOrderGroupHandle h,
                                                int64_t* actions_out,
                                                uint32_t max_actions);
  void flox_order_group_mark_action_dispatched(FloxOrderGroupHandle h, uint32_t leg_index,
                                               uint8_t kind);
  void flox_order_group_set_risk_limits(FloxOrderGroupHandle h,
                                        int64_t max_gross_notional_raw,
                                        double max_concentration_pct,
                                        int64_t max_leg_qty_raw);
  uint8_t flox_order_group_precheck_submission(FloxOrderGroupHandle h, double equity,
                                               const int64_t* market_ref_prices_raw,
                                               uint32_t market_ref_prices_len,
                                               char* rule_out, size_t rule_capacity,
                                               char* detail_out, size_t detail_capacity);
  void flox_order_group_set_pair_latency_budget_ns(FloxOrderGroupHandle h, int64_t budget_ns);
  uint8_t flox_order_group_pair_latency_decision(FloxOrderGroupHandle h,
                                                 int64_t leader_submit_ts_ns,
                                                 int64_t leader_ack_ts_ns,
                                                 uint8_t ack_received);

  // ============================================================
  // Multi-feed clock
  // ============================================================

  typedef void* FloxFeedClockHandle;

  FloxFeedClockHandle flox_feed_clock_create(const uint32_t* symbols, uint32_t symbol_count,
                                             uint8_t policy, int64_t timeout_ms,
                                             uint32_t leader_symbol,
                                             int64_t staleness_budget_ms);
  void flox_feed_clock_destroy(FloxFeedClockHandle h);

  uint32_t flox_feed_clock_symbol_count(FloxFeedClockHandle h);
  uint32_t flox_feed_clock_symbol_at(FloxFeedClockHandle h, uint32_t index);

  uint8_t flox_feed_clock_tick(FloxFeedClockHandle h, int64_t ts_ns, uint32_t symbol);

  uint8_t flox_feed_clock_last_fired(FloxFeedClockHandle h);
  uint32_t flox_feed_clock_last_triggered_by(FloxFeedClockHandle h);
  int64_t flox_feed_clock_last_seen_at(FloxFeedClockHandle h, uint32_t index);
  int64_t flox_feed_clock_staleness_at(FloxFeedClockHandle h, uint32_t index);

  void flox_feed_clock_reset(FloxFeedClockHandle h);

  // ============================================================
  // Position tracking
  // ============================================================

  FloxPositionTrackerHandle flox_position_tracker_create(uint8_t cost_basis);
  void flox_position_tracker_destroy(FloxPositionTrackerHandle tracker);
  void flox_position_tracker_on_fill(FloxPositionTrackerHandle tracker, uint32_t symbol,
                                     uint8_t side, double price, double quantity);
  double flox_position_tracker_position(FloxPositionTrackerHandle tracker, uint32_t symbol);
  double flox_position_tracker_avg_entry(FloxPositionTrackerHandle tracker, uint32_t symbol);
  double flox_position_tracker_realized_pnl(FloxPositionTrackerHandle tracker, uint32_t symbol);
  double flox_position_tracker_total_pnl(FloxPositionTrackerHandle tracker);

  // ============================================================
  // Volume profile
  // ============================================================

  FloxVolumeProfileHandle flox_volume_profile_create(double tick_size);
  void flox_volume_profile_destroy(FloxVolumeProfileHandle profile);
  void flox_volume_profile_add_trade(FloxVolumeProfileHandle profile, double price, double quantity,
                                     uint8_t is_buy);
  double flox_volume_profile_poc(FloxVolumeProfileHandle profile);
  double flox_volume_profile_vah(FloxVolumeProfileHandle profile);
  double flox_volume_profile_val(FloxVolumeProfileHandle profile);
  double flox_volume_profile_total_volume(FloxVolumeProfileHandle profile);
  double flox_volume_profile_total_delta(FloxVolumeProfileHandle profile);
  uint32_t flox_volume_profile_num_levels(FloxVolumeProfileHandle profile);
  void flox_volume_profile_clear(FloxVolumeProfileHandle profile);

  // ============================================================
  // Footprint bar
  // ============================================================

  FloxFootprintHandle flox_footprint_create(double tick_size);
  void flox_footprint_destroy(FloxFootprintHandle footprint);
  void flox_footprint_add_trade(FloxFootprintHandle footprint, double price, double quantity,
                                uint8_t is_buy);
  double flox_footprint_total_delta(FloxFootprintHandle footprint);
  double flox_footprint_total_volume(FloxFootprintHandle footprint);
  uint32_t flox_footprint_num_levels(FloxFootprintHandle footprint);
  void flox_footprint_clear(FloxFootprintHandle footprint);

  // ============================================================
  // Statistics
  // ============================================================

  double flox_stat_correlation(const double* x, const double* y, size_t len);
  double flox_stat_profit_factor(const double* pnl, size_t len);
  double flox_stat_win_rate(const double* pnl, size_t len);

  // ============================================================
  // Order tracker
  // ============================================================

  typedef void* FloxOrderTrackerHandle;

  FloxOrderTrackerHandle flox_order_tracker_create(void);
  void flox_order_tracker_destroy(FloxOrderTrackerHandle tracker);
  uint8_t flox_order_tracker_on_submitted(FloxOrderTrackerHandle tracker, uint64_t order_id,
                                          uint32_t symbol, uint8_t side, double price, double qty);
  uint8_t flox_order_tracker_on_filled(FloxOrderTrackerHandle tracker, uint64_t order_id,
                                       double fill_qty);
  uint8_t flox_order_tracker_on_canceled(FloxOrderTrackerHandle tracker, uint64_t order_id);
  uint8_t flox_order_tracker_is_active(FloxOrderTrackerHandle tracker, uint64_t order_id);
  uint32_t flox_order_tracker_active_count(FloxOrderTrackerHandle tracker);
  uint32_t flox_order_tracker_total_count(FloxOrderTrackerHandle tracker);
  void flox_order_tracker_prune(FloxOrderTrackerHandle tracker);

  // ============================================================
  // Position group tracker
  // ============================================================

  typedef void* FloxPositionGroupHandle;

  FloxPositionGroupHandle flox_position_group_create(void);
  void flox_position_group_destroy(FloxPositionGroupHandle tracker);
  uint64_t flox_position_group_open(FloxPositionGroupHandle tracker, uint64_t order_id,
                                    uint32_t symbol, uint8_t side, double price, double qty);
  void flox_position_group_close(FloxPositionGroupHandle tracker, uint64_t position_id,
                                 double exit_price);
  void flox_position_group_partial_close(FloxPositionGroupHandle tracker, uint64_t position_id,
                                         double qty, double exit_price);
  double flox_position_group_net_position(FloxPositionGroupHandle tracker, uint32_t symbol);
  double flox_position_group_realized_pnl(FloxPositionGroupHandle tracker, uint32_t symbol);
  double flox_position_group_total_pnl(FloxPositionGroupHandle tracker);
  uint32_t flox_position_group_open_count(FloxPositionGroupHandle tracker, uint32_t symbol);
  void flox_position_group_prune(FloxPositionGroupHandle tracker);

  // ============================================================
  // Market profile
  // ============================================================

  typedef void* FloxMarketProfileHandle;

  FloxMarketProfileHandle flox_market_profile_create(double tick_size, uint32_t period_minutes,
                                                     int64_t session_start_ns);
  void flox_market_profile_destroy(FloxMarketProfileHandle profile);
  void flox_market_profile_add_trade(FloxMarketProfileHandle profile, int64_t timestamp_ns,
                                     double price, double qty, uint8_t is_buy);
  double flox_market_profile_poc(FloxMarketProfileHandle profile);
  double flox_market_profile_vah(FloxMarketProfileHandle profile);
  double flox_market_profile_val(FloxMarketProfileHandle profile);
  double flox_market_profile_ib_high(FloxMarketProfileHandle profile);
  double flox_market_profile_ib_low(FloxMarketProfileHandle profile);
  uint8_t flox_market_profile_is_poor_high(FloxMarketProfileHandle profile);
  uint8_t flox_market_profile_is_poor_low(FloxMarketProfileHandle profile);
  uint32_t flox_market_profile_num_levels(FloxMarketProfileHandle profile);
  void flox_market_profile_clear(FloxMarketProfileHandle profile);

  // ============================================================
  // Composite book matrix
  // ============================================================

  typedef void* FloxCompositeBookHandle;

  FloxCompositeBookHandle flox_composite_book_create(void);
  void flox_composite_book_destroy(FloxCompositeBookHandle book);
  uint8_t flox_composite_book_best_bid(FloxCompositeBookHandle book, uint32_t symbol,
                                       double* price_out, double* qty_out);
  uint8_t flox_composite_book_best_ask(FloxCompositeBookHandle book, uint32_t symbol,
                                       double* price_out, double* qty_out);
  uint8_t flox_composite_book_has_arb(FloxCompositeBookHandle book, uint32_t symbol);
  void flox_composite_book_mark_stale(FloxCompositeBookHandle book, uint32_t exchange,
                                      uint32_t symbol);
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

  uint32_t flox_simulated_executor_get_fills(FloxSimulatedExecutorHandle executor, FloxFill* fills_out,
                                             uint32_t max_fills);

  // ============================================================
  // Additional bar aggregation
  // ============================================================

  uint32_t flox_aggregate_range_bars(const int64_t* timestamps, const double* prices,
                                     const double* quantities, const uint8_t* is_buy, size_t len,
                                     double range_size, FloxBar* bars_out, uint32_t max_bars);
  uint32_t flox_aggregate_renko_bars(const int64_t* timestamps, const double* prices,
                                     const double* quantities, const uint8_t* is_buy, size_t len,
                                     double brick_size, FloxBar* bars_out, uint32_t max_bars);

  // ============================================================
  // L3 Order book
  // ============================================================

  typedef void* FloxL3BookHandle;

  FloxL3BookHandle flox_l3_book_create(void);
  void flox_l3_book_destroy(FloxL3BookHandle book);
  int32_t flox_l3_book_add_order(FloxL3BookHandle book, uint64_t order_id, double price,
                                 double quantity, uint8_t side);
  int32_t flox_l3_book_remove_order(FloxL3BookHandle book, uint64_t order_id);
  int32_t flox_l3_book_modify_order(FloxL3BookHandle book, uint64_t order_id, double new_qty);
  uint8_t flox_l3_book_best_bid(FloxL3BookHandle book, double* price_out);
  uint8_t flox_l3_book_best_ask(FloxL3BookHandle book, double* price_out);
  double flox_l3_book_bid_at_price(FloxL3BookHandle book, double price);
  double flox_l3_book_ask_at_price(FloxL3BookHandle book, double price);

  // ============================================================
  // Data writer (binary log)
  // ============================================================

  typedef void* FloxDataWriterHandle;

  FloxDataWriterHandle flox_data_writer_create(const char* output_dir, uint64_t max_segment_mb,
                                               uint8_t exchange_id);
  void flox_data_writer_destroy(FloxDataWriterHandle writer);
  uint8_t flox_data_writer_write_trade(FloxDataWriterHandle writer, int64_t exchange_ts_ns,
                                       int64_t recv_ts_ns, double price, double qty,
                                       uint64_t trade_id, uint32_t symbol_id, uint8_t side);
  void flox_data_writer_flush(FloxDataWriterHandle writer);
  void flox_data_writer_close(FloxDataWriterHandle writer);

  // ============================================================
  // Data reader (binary log)
  // ============================================================

  typedef void* FloxDataReaderHandle;

  FloxDataReaderHandle flox_data_reader_create(const char* data_dir);
  void flox_data_reader_destroy(FloxDataReaderHandle reader);
  uint64_t flox_data_reader_count(FloxDataReaderHandle reader);

  // ============================================================
  // Order book level access
  // ============================================================

  uint32_t flox_book_get_bids(FloxBookHandle book, double* prices_out, double* qtys_out,
                              uint32_t max_levels);
  uint32_t flox_book_get_asks(FloxBookHandle book, double* prices_out, double* qtys_out,
                              uint32_t max_levels);

  // ============================================================
  // Heikin-Ashi bar aggregation
  // ============================================================

  uint32_t flox_aggregate_heikin_ashi_bars(const int64_t* timestamps, const double* prices,
                                           const double* quantities, const uint8_t* is_buy,
                                           size_t len, double interval_seconds, FloxBar* bars_out,
                                           uint32_t max_bars);

  // ============================================================
  // Additional stats
  // ============================================================

  double flox_stat_permutation_test(const double* group1, size_t len1, const double* group2,
                                    size_t len2, uint32_t num_permutations);
  void flox_stat_bootstrap_ci(const double* data, size_t len, double confidence,
                              uint32_t num_samples, double* lower_out, double* median_out,
                              double* upper_out);
  void flox_stat_whites_reality_check(const double* returns, size_t num_strategies,
                                      size_t num_periods, uint32_t num_bootstrap,
                                      double avg_block_size, double* p_value_out,
                                      double* best_stat_out, int32_t* best_index_out);

  // ============================================================
  // Segment operations
  // ============================================================

  uint8_t flox_segment_validate(const char* path);
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
    FLOX_QUEUE_FULL = 2
  } FloxQueueModel;

  // Configure slippage. Applies to market-style fills on all symbols unless
  // a per-symbol override is set. `tick_size` is the venue tick size in
  // price units (e.g. 0.01 for 1-cent ticks); pass 0.0 to fall back to one
  // raw price unit.
  void flox_simulated_executor_set_default_slippage(FloxSimulatedExecutorHandle executor,
                                                    int32_t model, int32_t ticks,
                                                    double tick_size, double bps,
                                                    double impact_coeff);
  void flox_simulated_executor_set_symbol_slippage(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                                   int32_t model, int32_t ticks,
                                                   double tick_size, double bps,
                                                   double impact_coeff);

  // Configure queue simulation for limit orders.
  void flox_simulated_executor_set_queue_model(FloxSimulatedExecutorHandle executor, int32_t model,
                                               uint32_t depth);

  // Feed a trade with quantity (enables queue-fill simulation for limit orders).
  void flox_simulated_executor_on_trade_qty(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                            double price, double quantity, uint8_t is_buy);

  // Feed a top-of-book snapshot (both best bid and best ask in one call).
  // For multi-level updates, build a BookUpdate on the C++ side; the C API
  // intentionally does not expose a stateful per-side helper because that
  // makes it too easy to accidentally clear the opposite side.
  void flox_simulated_executor_on_best_levels(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                              double bid_price, double bid_qty, double ask_price,
                                              double ask_qty);

  // Feed a full L2 snapshot with parallel bid/ask arrays.
  void flox_simulated_executor_on_book_snapshot(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                                const double* bid_prices, const double* bid_qtys,
                                                uint32_t n_bids, const double* ask_prices,
                                                const double* ask_qtys, uint32_t n_asks);

  // BacktestResult handle: aggregates fills into trades + stats + equity curve.
  typedef void* FloxBacktestResultHandle;

  FloxBacktestResultHandle flox_backtest_result_create(double initial_capital,
                                                       double fee_rate,
                                                       uint8_t use_percentage_fee,
                                                       double fixed_fee_per_trade,
                                                       double risk_free_rate,
                                                       double annualization_factor);
  void flox_backtest_result_destroy(FloxBacktestResultHandle result);

  // Feed fills produced by a SimulatedExecutor. Fills are processed in order.
  void flox_backtest_result_record_fill(FloxBacktestResultHandle result,
                                        uint64_t order_id, uint32_t symbol, uint8_t side,
                                        double price, double quantity, int64_t timestamp_ns);

  // Drain all fills from a SimulatedExecutor into a BacktestResult in FIFO order.
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

  void flox_backtest_result_stats(FloxBacktestResultHandle result, FloxBacktestStats* out);

  typedef struct
  {
    int64_t timestamp_ns;
    double equity;
    double drawdown_pct;
  } FloxEquityPoint;

  // Returns total available points. If points_out is non-NULL, writes up to
  // max_points entries and returns the number written.
  uint32_t flox_backtest_result_equity_curve(FloxBacktestResultHandle result,
                                             FloxEquityPoint* points_out,
                                             uint32_t max_points);

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
  FloxMergeResult flox_segment_merge_full(const char* input_paths, size_t num_paths,
                                          const char* output_dir, const char* output_name,
                                          uint8_t sort);

  FloxMergeResult flox_segment_merge_dir(const char* input_dir, const char* output_dir);

  FloxSplitResult flox_segment_split(const char* input_path, const char* output_dir,
                                     uint8_t mode, int64_t time_interval_ns,
                                     uint64_t events_per_file);

  FloxExportResult flox_segment_export(const char* input_path, const char* output_path,
                                       uint8_t format, int64_t from_ns, int64_t to_ns,
                                       const uint32_t* symbols, uint32_t num_symbols);

  uint8_t flox_segment_recompress(const char* input_path, const char* output_path,
                                  uint8_t compression);

  uint64_t flox_segment_extract_symbols(const char* input_path, const char* output_path,
                                        const uint32_t* symbols, uint32_t num_symbols);

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

  FloxDataReaderHandle flox_data_reader_create_filtered(const char* data_dir, int64_t from_ns,
                                                        int64_t to_ns, const uint32_t* symbols,
                                                        uint32_t num_symbols);

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
  uint64_t flox_data_reader_read_bbo(FloxDataReaderHandle reader, FloxBBO* bbos_out,
                                     uint64_t max_events);

  // Counts book update events and total levels in a single pass.
  // Returns event count; writes total level count to *total_levels_out (may be NULL).
  uint64_t flox_data_reader_count_book_updates(FloxDataReaderHandle reader,
                                               uint64_t* total_levels_out);

  // Reads book updates into pre-sized arrays. Caller must size headers_out
  // (>= event count) and levels_out (>= total levels) using
  // flox_data_reader_count_book_updates().
  // Returns number of events read.
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

  uint64_t flox_data_reader_read_trades_from(FloxDataReaderHandle reader,
                                             int64_t start_ts_ns,
                                             FloxTradeRecord* trades_out,
                                             uint64_t max_trades);

  uint64_t flox_data_reader_read_bbo_from(FloxDataReaderHandle reader,
                                          int64_t start_ts_ns,
                                          FloxBBO* bbos_out,
                                          uint64_t max_events);

  uint64_t flox_data_reader_count_book_updates_from(FloxDataReaderHandle reader,
                                                    int64_t start_ts_ns,
                                                    uint64_t* total_levels_out);

  uint64_t flox_data_reader_read_book_updates_from(FloxDataReaderHandle reader,
                                                   int64_t start_ts_ns,
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

  FloxWriterStats flox_data_writer_stats(FloxDataWriterHandle writer);

  // ============================================================
  // DataRecorder
  // ============================================================

  typedef void* FloxDataRecorderHandle;

  FloxDataRecorderHandle flox_data_recorder_create(const char* output_dir,
                                                   const char* exchange_name,
                                                   uint64_t max_segment_mb);
  void flox_data_recorder_destroy(FloxDataRecorderHandle recorder);
  void flox_data_recorder_add_symbol(FloxDataRecorderHandle recorder, uint32_t symbol_id,
                                     const char* name, const char* base, const char* quote,
                                     int8_t price_precision, int8_t qty_precision);
  void flox_data_recorder_start(FloxDataRecorderHandle recorder);
  void flox_data_recorder_stop(FloxDataRecorderHandle recorder);
  void flox_data_recorder_flush(FloxDataRecorderHandle recorder);
  uint8_t flox_data_recorder_is_recording(FloxDataRecorderHandle recorder);

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

  FloxPartitionerHandle flox_partitioner_create(const char* data_dir);
  void flox_partitioner_destroy(FloxPartitionerHandle partitioner);

  // All partition functions return number of partitions.
  // If partitions_out is NULL, returns count only.
  uint32_t flox_partitioner_by_time(FloxPartitionerHandle p, uint32_t num_partitions,
                                    int64_t warmup_ns, FloxPartition* partitions_out,
                                    uint32_t max_partitions);
  uint32_t flox_partitioner_by_duration(FloxPartitionerHandle p, int64_t duration_ns,
                                        int64_t warmup_ns, FloxPartition* partitions_out,
                                        uint32_t max_partitions);
  uint32_t flox_partitioner_by_calendar(FloxPartitionerHandle p, uint8_t unit,
                                        int64_t warmup_ns, FloxPartition* partitions_out,
                                        uint32_t max_partitions);
  uint32_t flox_partitioner_by_symbol(FloxPartitionerHandle p, uint32_t num_partitions,
                                      FloxPartition* partitions_out, uint32_t max_partitions);
  uint32_t flox_partitioner_per_symbol(FloxPartitionerHandle p, FloxPartition* partitions_out,
                                       uint32_t max_partitions);
  uint32_t flox_partitioner_by_event_count(FloxPartitionerHandle p, uint32_t num_partitions,
                                           FloxPartition* partitions_out,
                                           uint32_t max_partitions);

  // ============================================================
  // Pointer-out wrappers for struct-returning functions.
  // These exist for language bindings (Codon, QuickJS) that cannot
  // consume C structs returned by value via their FFI.
  // Each writes sizeof(OriginalStruct) bytes to *out.
  // ============================================================

  void flox_data_reader_summary_p(FloxDataReaderHandle reader, void* out);
  void flox_data_reader_stats_p(FloxDataReaderHandle reader, void* out);
  void flox_data_writer_stats_p(FloxDataWriterHandle writer, void* out);
  void flox_segment_merge_full_p(const char* input_paths, size_t num_paths,
                                 const char* output_dir, const char* output_name,
                                 uint8_t sort, void* out);
  void flox_segment_merge_dir_p(const char* input_dir, const char* output_dir, void* out);
  void flox_segment_split_p(const char* input_path, const char* output_dir, uint8_t mode,
                            int64_t time_interval_ns, uint64_t events_per_file, void* out);
  void flox_segment_export_p(const char* input_path, const char* output_path, uint8_t format,
                             int64_t from_ns, int64_t to_ns,
                             const uint32_t* symbols, uint32_t num_symbols, void* out);
  void flox_segment_validate_full_p(const char* path, uint8_t verify_crc,
                                    uint8_t verify_timestamps, void* out);
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

  FloxRiskManagerHandle flox_risk_manager_create(FloxRiskManagerCallbacks callbacks);
  FloxRiskManagerHandle flox_risk_manager_create_p(const FloxRiskManagerCallbacks* callbacks);
  void flox_risk_manager_destroy(FloxRiskManagerHandle rm);

  // ============================================================
  // KillSwitch — global halt hook. Fires before OrderValidator and
  // RiskManager. When `check` returns 0, the signal is dropped and
  // downstream hooks are skipped. NULL `check` is a no-op.
  // ============================================================

  typedef uint8_t (*FloxKillSwitchCheckFn)(void* user_data,
                                           const FloxSignal* signal);

  typedef struct
  {
    FloxKillSwitchCheckFn check;
    void* user_data;
  } FloxKillSwitchCallbacks;

  typedef void* FloxKillSwitchHandle;

  FloxKillSwitchHandle flox_kill_switch_create(FloxKillSwitchCallbacks callbacks);
  FloxKillSwitchHandle flox_kill_switch_create_p(const FloxKillSwitchCallbacks* callbacks);
  void flox_kill_switch_destroy(FloxKillSwitchHandle ks);

  // ============================================================
  // OrderValidator — sanity check. Fires after KillSwitch, before
  // RiskManager. When `validate` returns 0, the signal is dropped and
  // RiskManager is skipped. NULL `validate` is a no-op.
  // ============================================================

  typedef uint8_t (*FloxOrderValidatorValidateFn)(void* user_data,
                                                  const FloxSignal* signal);

  typedef struct
  {
    FloxOrderValidatorValidateFn validate;
    void* user_data;
  } FloxOrderValidatorCallbacks;

  typedef void* FloxOrderValidatorHandle;

  FloxOrderValidatorHandle flox_order_validator_create(FloxOrderValidatorCallbacks callbacks);
  FloxOrderValidatorHandle flox_order_validator_create_p(const FloxOrderValidatorCallbacks* callbacks);
  void flox_order_validator_destroy(FloxOrderValidatorHandle ov);

  // ============================================================
  // Logger — process-wide log redirection callback.
  //
  // FLOX_LOG_INFO / WARN / ERROR macros normally write to stderr via
  // the bundled ConsoleLogger. Setting a callback here redirects every
  // subsequent log call to the binding's language-native logger.
  //
  // `level`: 0 = Info, 1 = Warn, 2 = Error.
  // `msg`:   null-terminated UTF-8 string; valid only for the duration
  //          of the callback. Copy if retained.
  //
  // Single global setter — pass NULL to restore the default ConsoleLogger.
  // Thread-safe: callbacks may fire from any consumer thread, so the
  // user-supplied callback must itself be thread-safe.
  // ============================================================

  typedef void (*FloxLogCallback)(void* user_data, int32_t level, const char* msg);

  void flox_set_log_callback(FloxLogCallback callback, void* user_data);

  // ============================================================
  // PnLTracker — post-emission observer (fires after on_signal,
  // never blocks). Attach to a runner / engine to shadow-track
  // exposure based on emitted signals. NULL detaches.
  // ============================================================

  typedef void (*FloxPnLTrackerOnSignalFn)(void* user_data, const FloxSignal* signal);

  typedef struct
  {
    FloxPnLTrackerOnSignalFn on_signal;
    void* user_data;
  } FloxPnLTrackerCallbacks;

  typedef void* FloxPnLTrackerHandle;

  FloxPnLTrackerHandle flox_pnl_tracker_create(FloxPnLTrackerCallbacks callbacks);
  FloxPnLTrackerHandle flox_pnl_tracker_create_p(const FloxPnLTrackerCallbacks* callbacks);
  void flox_pnl_tracker_destroy(FloxPnLTrackerHandle tracker);

  // ============================================================
  // StorageSink — same shape as PnLTracker; persist every emitted
  // signal to the binding's storage. Fires after PnLTracker.
  // ============================================================

  typedef void (*FloxStorageSinkStoreFn)(void* user_data, const FloxSignal* signal);

  typedef struct
  {
    FloxStorageSinkStoreFn store;
    void* user_data;
  } FloxStorageSinkCallbacks;

  typedef void* FloxStorageSinkHandle;

  FloxStorageSinkHandle flox_storage_sink_create(FloxStorageSinkCallbacks callbacks);
  FloxStorageSinkHandle flox_storage_sink_create_p(const FloxStorageSinkCallbacks* callbacks);
  void flox_storage_sink_destroy(FloxStorageSinkHandle sink);

  // ============================================================
  // MarketDataRecorder — receive every market data event fed into the
  // engine, for custom recording in the host language.
  //
  // Companion to flox_data_recorder_* (in-tree binary log writer); this
  // hook lets a binding implement IMarketDataRecorder. Any callback may
  // be NULL (no-op). Pointers and arrays are valid only for the duration
  // of the callback; copy if retained.
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

  FloxMarketDataRecorderHandle flox_market_data_recorder_create(
      FloxMarketDataRecorderCallbacks callbacks);
  FloxMarketDataRecorderHandle flox_market_data_recorder_create_p(
      const FloxMarketDataRecorderCallbacks* callbacks);
  void flox_market_data_recorder_destroy(FloxMarketDataRecorderHandle recorder);

  // ============================================================
  // ReplaySource — binding-side market data event source for backtests.
  //
  // Bindings provide a custom event reader. The engine pulls events by
  // calling `next` repeatedly until it returns 0. For book events, the
  // binding sets `bids` / `asks` to point at its own buffer; pointers
  // must remain valid until the next `next` call.
  // ============================================================

  // Tagged event: 1=Trade, 2=BookSnapshot, 3=BookDelta. Matches
  // flox::replay::EventType.
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
    const FloxBookLevel* bids;
    const FloxBookLevel* asks;
  } FloxReplayEvent;

  typedef uint8_t (*FloxReplaySourceNextFn)(void* user_data, FloxReplayEvent* event_out);
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

  FloxReplaySourceHandle flox_replay_source_create(FloxReplaySourceCallbacks callbacks);
  FloxReplaySourceHandle flox_replay_source_create_p(const FloxReplaySourceCallbacks* callbacks);
  void flox_replay_source_destroy(FloxReplaySourceHandle source);

  // Convenience: forward a seek to the binding's seek_to callback.
  uint8_t flox_replay_source_seek_to(FloxReplaySourceHandle source, int64_t timestamp_ns);

  // ============================================================
  // ExecutionListener — observe order lifecycle events.
  //
  // Bindings register callbacks that fire as orders move through the
  // execution path (submitted → accepted → filled / canceled / rejected
  // / etc.). Used as a backtest observer (SimulatedExecutor inside
  // BacktestRunner emits these) or a live-broker fill surface.
  //
  // The order pointer is read-only and valid only for the duration of
  // the callback; copy fields you need to retain. Any callback may be
  // NULL (no-op).
  // ============================================================

  typedef struct
  {
    uint64_t id;
    uint64_t client_order_id;
    uint32_t symbol;
    uint16_t strategy_id;
    uint16_t order_tag;
    uint8_t side;           // 0=BUY, 1=SELL
    uint8_t type;           // OrderType enum
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
    void* user_data;
  } FloxExecutionListenerCallbacks;

  typedef void* FloxExecutionListenerHandle;

  FloxExecutionListenerHandle
  flox_execution_listener_create(FloxExecutionListenerCallbacks callbacks);
  FloxExecutionListenerHandle
  flox_execution_listener_create_p(const FloxExecutionListenerCallbacks* callbacks);
  void flox_execution_listener_destroy(FloxExecutionListenerHandle listener);

  // ============================================================
  // Executor — binding-supplied IOrderExecutor.
  //
  // Bindings provide an executor that places, cancels, replaces and
  // OCO-submits orders on a real broker (or on a custom simulator). The
  // engine routes every signal through this executor instead of the
  // built-in SimulatedExecutor when one is attached.
  //
  // The concrete in-process SimulatedExecutor is exposed separately
  // as flox_simulated_executor_*.
  // ============================================================

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

  FloxExecutorHandle flox_executor_create(FloxExecutorCallbacks callbacks);
  FloxExecutorHandle flox_executor_create_p(const FloxExecutorCallbacks* callbacks);
  void flox_executor_destroy(FloxExecutorHandle executor);

  // Query the executor's reported capabilities. Forwards to the binding's
  // capabilities() callback. caps_out is zeroed if no callback registered.
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

  FloxLiveEngineHandle flox_live_engine_create(FloxRegistryHandle registry);
  void flox_live_engine_destroy(FloxLiveEngineHandle engine);

  // Attach (or detach with rm = NULL) a risk manager to this engine. The
  // risk manager's `allow` callback fires on every signal before it reaches
  // the user-supplied on_signal callback. Safe to call before or after
  // start(); the engine takes a non-owning reference.
  void flox_live_engine_set_risk_manager(FloxLiveEngineHandle engine,
                                         FloxRiskManagerHandle rm);

  // KillSwitch / OrderValidator. Evaluation order on every signal:
  // KillSwitch → OrderValidator → RiskManager.
  void flox_live_engine_set_kill_switch(FloxLiveEngineHandle engine,
                                        FloxKillSwitchHandle ks);
  void flox_live_engine_set_order_validator(FloxLiveEngineHandle engine,
                                            FloxOrderValidatorHandle ov);

  // Post-emission observers. PnLTracker → StorageSink, after on_signal.
  void flox_live_engine_set_pnl_tracker(FloxLiveEngineHandle engine,
                                        FloxPnLTrackerHandle tracker);
  void flox_live_engine_set_storage_sink(FloxLiveEngineHandle engine,
                                         FloxStorageSinkHandle sink);

  // Attach (or detach with NULL) a market data recorder. Fires on every
  // published trade and book update; on_start / on_stop fire on engine
  // start/stop while attached.
  void flox_live_engine_set_market_data_recorder(FloxLiveEngineHandle engine,
                                                 FloxMarketDataRecorderHandle recorder);

  // Attach (or detach with NULL) a binding-supplied executor. When set,
  // emitted signals are forwarded to the executor (submit / cancel /
  // replace / cancel_all / submit_oco) instead of relying on the user's
  // on_signal callback to route orders. on_start / on_stop fire balanced
  // against engine start/stop.
  void flox_live_engine_set_executor(FloxLiveEngineHandle engine,
                                     FloxExecutorHandle executor);

  // Attach a strategy to both TradeBus and BookUpdateBus.
  // on_signal is called from the consumer thread when the strategy emits an order.
  // The caller must ensure thread safety when submitting orders from on_signal.
  void flox_live_engine_add_strategy(FloxLiveEngineHandle engine,
                                     FloxStrategyHandle strategy,
                                     FloxOnSignalCallback on_signal,
                                     void* user_data);

  void flox_live_engine_start(FloxLiveEngineHandle engine);
  void flox_live_engine_stop(FloxLiveEngineHandle engine);

  // Publish a trade tick to the TradeBus.
  // Lock-free. Returns immediately; consumer threads process asynchronously.
  void flox_live_engine_publish_trade(FloxLiveEngineHandle engine,
                                      uint32_t symbol,
                                      double price, double qty, uint8_t is_buy,
                                      int64_t exchange_ts_ns);

  // Publish a full L2 book snapshot to the BookUpdateBus.
  // Lock-free. Returns immediately; consumer threads process asynchronously.
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

  FloxRunnerHandle flox_runner_create(FloxRegistryHandle registry,
                                      FloxOnSignalCallback on_signal,
                                      void* user_data);
  void flox_runner_destroy(FloxRunnerHandle runner);

  // Attach a strategy (created via flox_strategy_create) to the runner.
  // The runner does NOT take ownership; call flox_strategy_destroy separately.
  void flox_runner_add_strategy(FloxRunnerHandle runner, FloxStrategyHandle strategy);

  // Attach (or detach with rm = NULL) a risk manager to this runner.
  // Same semantics as flox_live_engine_set_risk_manager.
  void flox_runner_set_risk_manager(FloxRunnerHandle runner,
                                    FloxRiskManagerHandle rm);

  // KillSwitch / OrderValidator setters. Evaluation order on every signal:
  // KillSwitch → OrderValidator → RiskManager.
  void flox_runner_set_kill_switch(FloxRunnerHandle runner,
                                   FloxKillSwitchHandle ks);
  void flox_runner_set_order_validator(FloxRunnerHandle runner,
                                       FloxOrderValidatorHandle ov);

  // Post-emission observers. PnLTracker → StorageSink, after on_signal.
  void flox_runner_set_pnl_tracker(FloxRunnerHandle runner,
                                   FloxPnLTrackerHandle tracker);
  void flox_runner_set_storage_sink(FloxRunnerHandle runner,
                                    FloxStorageSinkHandle sink);

  // Attach (or detach with NULL) a market data recorder. on_trade and
  // on_book_update fire synchronously from flox_runner_on_trade /
  // flox_runner_on_book_snapshot. on_start / on_stop fire on runner
  // start/stop.
  void flox_runner_set_market_data_recorder(FloxRunnerHandle runner,
                                            FloxMarketDataRecorderHandle recorder);

  // Attach (or detach with NULL) a binding-supplied executor. When set,
  // emitted signals are forwarded to the executor in addition to the
  // user's on_signal callback. on_start / on_stop fire balanced against
  // runner start/stop.
  void flox_runner_set_executor(FloxRunnerHandle runner,
                                FloxExecutorHandle executor);

  void flox_runner_start(FloxRunnerHandle runner);
  void flox_runner_stop(FloxRunnerHandle runner);

  // Push a trade tick. Strategy on_trade callbacks fire synchronously.
  void flox_runner_on_trade(FloxRunnerHandle runner, uint32_t symbol,
                            double price, double qty, uint8_t is_buy,
                            int64_t exchange_ts_ns);

  // Push a full L2 book snapshot. Strategy on_book callbacks fire synchronously.
  void flox_runner_on_book_snapshot(FloxRunnerHandle runner, uint32_t symbol,
                                    const double* bid_prices, const double* bid_qtys,
                                    uint32_t n_bids,
                                    const double* ask_prices, const double* ask_qtys,
                                    uint32_t n_asks,
                                    int64_t exchange_ts_ns);

  // Push a closed OHLC bar. Strategy on_bar callbacks fire synchronously.
  // bar_type / bar_type_param / close_reason match FloxBarData.
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

  FloxBacktestRunnerHandle flox_backtest_runner_create(FloxRegistryHandle registry,
                                                       double fee_rate,
                                                       double initial_capital);
  void flox_backtest_runner_destroy(FloxBacktestRunnerHandle runner);

  // Attach a strategy. BacktestRunner becomes the signal handler — emitted
  // orders are routed to SimulatedExecutor automatically.
  void flox_backtest_runner_set_strategy(FloxBacktestRunnerHandle runner,
                                         FloxStrategyHandle strategy);

  // Replay a CSV file (columns: timestamp, open, high, low, close, volume).
  // Returns 1 on success, 0 on error.
  int flox_backtest_runner_run_csv(FloxBacktestRunnerHandle runner,
                                   const char* path,
                                   const char* symbol,
                                   FloxBacktestStats* stats_out);

  // Drive the backtest off a `.floxlog` tape directory.
  // Returns 1 on success, 0 on error.
  int flox_backtest_runner_run_tape(FloxBacktestRunnerHandle runner,
                                    const char* tape_dir,
                                    FloxBacktestStats* stats_out);

  // Replay raw OHLCV arrays (timestamps in nanoseconds, close prices as double).
  // Each row produces one synthetic trade (price=close, qty=1). Strategy.on_trade fires.
  // Returns 1 on success, 0 on error.
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
  // Fires source.on_start before reading and source.on_stop after the
  // runner has drained the stream. Returns 1 on success, 0 on error.
  int flox_backtest_runner_run_replay_source(FloxBacktestRunnerHandle runner,
                                             FloxReplaySourceHandle source,
                                             FloxBacktestStats* stats_out);

  // Returns a NEW BacktestResult handle that owns a copy of the runner's
  // last completed result, or NULL if no run has happened yet. Caller
  // takes ownership and must free with flox_backtest_result_destroy().
  // Stable across subsequent runs of the same runner — each run
  // overwrites the runner's internal copy, but already-taken handles
  // stay valid until destroyed.
  FloxBacktestResultHandle flox_backtest_runner_take_result(FloxBacktestRunnerHandle runner);

  // Attach a binding-side execution listener to BacktestRunner. Multiple
  // listeners may be attached. Caller retains ownership of the listener.
  void flox_backtest_runner_add_execution_listener(FloxBacktestRunnerHandle runner,
                                                   FloxExecutionListenerHandle listener);

  // Attach (or detach with NULL) a binding-supplied executor. When set,
  // emitted signals are routed to the executor instead of the built-in
  // SimulatedExecutor.
  void flox_backtest_runner_set_executor(FloxBacktestRunnerHandle runner,
                                         FloxExecutorHandle executor);

  // Pre-trade gate parity with the live runner. All four hooks are
  // optional (NULL = no-op). Reduce-only orders bypass the gate so a
  // tightening cap cannot strand a strategy in a position.
  void flox_backtest_runner_set_risk_manager(FloxBacktestRunnerHandle runner,
                                             FloxRiskManagerHandle rm);
  void flox_backtest_runner_set_kill_switch(FloxBacktestRunnerHandle runner,
                                            FloxKillSwitchHandle ks);
  void flox_backtest_runner_set_order_validator(FloxBacktestRunnerHandle runner,
                                                FloxOrderValidatorHandle ov);
  void flox_backtest_runner_set_pnl_tracker(FloxBacktestRunnerHandle runner,
                                            FloxPnLTrackerHandle tracker);

  // ============================================================
  // Walk-forward
  // ============================================================

  typedef struct
  {
    uint8_t mode;
    uint64_t train_size;
    uint64_t test_size;
    uint64_t step;
    uint64_t min_train_size;
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

  typedef FloxStrategyHandle (*FloxWalkForwardFactoryFn)(
      void* user_data, uint64_t fold_index);

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

  typedef void* FloxGridSearchHandle;

  typedef int (*FloxGridSearchFactoryFn)(
      void* user_data, uint64_t param_index,
      const double* params, uint32_t num_params,
      FloxBacktestStats* out_stats);

  FloxGridSearchHandle flox_grid_search_create();
  void flox_grid_search_destroy(FloxGridSearchHandle gs);
  void flox_grid_search_add_axis(FloxGridSearchHandle gs,
                                 const double* values, uint32_t num_values);
  uint64_t flox_grid_search_total(FloxGridSearchHandle gs);
  uint32_t flox_grid_search_params_for_index(FloxGridSearchHandle gs,
                                             uint64_t index,
                                             double* params_out,
                                             uint32_t max_params);
  uint64_t flox_grid_search_run(FloxGridSearchHandle gs,
                                FloxGridSearchFactoryFn factory,
                                void* user_data,
                                FloxBacktestStats* stats_out,
                                uint32_t max_results);

  // ============================================================
  // Heatmap rendering
  // ============================================================

  typedef struct
  {
    const double* z;
    uint32_t rows;
    uint32_t cols;
    const char* const* row_labels;
    uint32_t num_row_labels;
    const char* const* col_labels;
    uint32_t num_col_labels;
    const char* title;
    const char* x_axis_name;
    const char* y_axis_name;
    const char* metric_name;
  } FloxHeatmapData;

  uint64_t flox_render_heatmap_html(const FloxHeatmapData* data,
                                    char* out_buf, uint64_t max_size);

  // ============================================================
  // Latency models (sampling primitive for backtest realism)
  // ============================================================
  //
  // Each model is created via a typed factory and queried through the
  // shared flox_latency_* accessors. Construction returns NULL on
  // invalid input (negative means/stddevs, empty empirical inputs).
  // All delays are non-negative nanoseconds.

  typedef void* FloxLatencyModelHandle;

  typedef struct
  {
    int64_t feed_ns;
    int64_t order_ns;
    int64_t fill_ns;
  } FloxLatencySample;

  FloxLatencyModelHandle flox_latency_constant_create(int64_t feed_ns,
                                                      int64_t order_ns,
                                                      int64_t fill_ns);

  FloxLatencyModelHandle flox_latency_gaussian_create(double feed_mean_ns,
                                                      double feed_stddev_ns,
                                                      double order_mean_ns,
                                                      double order_stddev_ns,
                                                      double fill_mean_ns,
                                                      double fill_stddev_ns,
                                                      uint64_t seed);

  FloxLatencyModelHandle flox_latency_exponential_create(double feed_mean_ns,
                                                         double order_mean_ns,
                                                         double fill_mean_ns,
                                                         uint64_t seed);

  // Each samples array is copied; pass NULL with count 0 if a
  // component should always return 0.
  FloxLatencyModelHandle flox_latency_empirical_create(const int64_t* feed_samples,
                                                       size_t feed_count,
                                                       const int64_t* order_samples,
                                                       size_t order_count,
                                                       const int64_t* fill_samples,
                                                       size_t fill_count,
                                                       uint64_t seed);

  void flox_latency_destroy(FloxLatencyModelHandle model);
  int64_t flox_latency_feed_delay(FloxLatencyModelHandle model);
  int64_t flox_latency_order_delay(FloxLatencyModelHandle model);
  int64_t flox_latency_fill_delay(FloxLatencyModelHandle model);
  void flox_latency_sample(FloxLatencyModelHandle model, FloxLatencySample* out);
  void flox_latency_reset(FloxLatencyModelHandle model, uint64_t seed);

  // ============================================================
  // Tape diff (replay-equivalence localization)
  // ============================================================

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

  FloxTapeDiffHandle flox_tape_diff_create(const char* left_path,
                                           const char* right_path,
                                           uint32_t max_mismatches,
                                           int64_t field_tolerance_ns);
  void flox_tape_diff_destroy(FloxTapeDiffHandle handle);
  uint64_t flox_tape_diff_left_count(FloxTapeDiffHandle handle);
  uint64_t flox_tape_diff_right_count(FloxTapeDiffHandle handle);
  uint8_t flox_tape_diff_first_divergence(FloxTapeDiffHandle handle, uint64_t* out_index);
  uint8_t flox_tape_diff_equal(FloxTapeDiffHandle handle);
  uint64_t flox_tape_diff_mismatch_count(FloxTapeDiffHandle handle);
  uint64_t flox_tape_diff_copy_mismatches(FloxTapeDiffHandle handle,
                                          FloxTapeDiffMismatch* out,
                                          uint64_t max_entries);

  // ============================================================
  // Portfolio risk aggregator
  // ============================================================

  typedef void* FloxPortfolioRiskHandle;

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

  typedef struct
  {
    const char* rule;
    double value;
    double limit;
    const char* detail;
  } FloxBreach;

  FloxPortfolioRiskHandle flox_portfolio_risk_create(const FloxPortfolioRiskRules* rules,
                                                     double initial_equity);
  void flox_portfolio_risk_destroy(FloxPortfolioRiskHandle handle);
  void flox_portfolio_risk_update(FloxPortfolioRiskHandle handle, const char* name,
                                  const FloxStrategyAccountFields* fields, uint8_t field_mask);
  void flox_portfolio_risk_remove(FloxPortfolioRiskHandle handle, const char* name);
  void flox_portfolio_risk_reset_kill_switch(FloxPortfolioRiskHandle handle);
  uint8_t flox_portfolio_risk_check_order(FloxPortfolioRiskHandle handle,
                                          const char* strategy, double notional,
                                          const char* side, FloxBreach* out_breach);
  double flox_portfolio_risk_total_daily_pnl(FloxPortfolioRiskHandle handle);
  double flox_portfolio_risk_total_gross_exposure(FloxPortfolioRiskHandle handle);
  double flox_portfolio_risk_current_equity(FloxPortfolioRiskHandle handle);
  double flox_portfolio_risk_drawdown_pct(FloxPortfolioRiskHandle handle);
  uint8_t flox_portfolio_risk_kill_switch_active(FloxPortfolioRiskHandle handle);
  uint64_t flox_portfolio_risk_breach_count(FloxPortfolioRiskHandle handle);
  uint8_t flox_portfolio_risk_breach_at(FloxPortfolioRiskHandle handle, uint64_t index,
                                        FloxBreach* out);
  uint64_t flox_portfolio_risk_account_count(FloxPortfolioRiskHandle handle);

  // ============================================================
  // Execution algorithms (TWAP / VWAP / Iceberg / POV)
  // ============================================================

  typedef void* FloxExecAlgoHandle;

  typedef struct
  {
    uint64_t order_id;
    int64_t timestamp_ns;
    double qty;
    double price;
    uint8_t type;
  } FloxExecChildOrder;

  FloxExecAlgoHandle flox_exec_twap_create(double target_qty, uint8_t side,
                                           uint32_t symbol, uint8_t type,
                                           double limit_price,
                                           int64_t duration_ns,
                                           uint32_t slice_count,
                                           int64_t start_time_ns);
  FloxExecAlgoHandle flox_exec_vwap_create(double target_qty, uint8_t side,
                                           uint32_t symbol, uint8_t type,
                                           double limit_price,
                                           const int64_t* volume_curve_ts,
                                           const double* volume_curve_vol,
                                           size_t n);
  FloxExecAlgoHandle flox_exec_iceberg_create(double target_qty, uint8_t side,
                                              uint32_t symbol, uint8_t type,
                                              double limit_price,
                                              double visible_qty);
  FloxExecAlgoHandle flox_exec_pov_create(double target_qty, uint8_t side,
                                          uint32_t symbol, uint8_t type,
                                          double limit_price,
                                          double participation_rate,
                                          double min_slice_qty);
  void flox_exec_destroy(FloxExecAlgoHandle handle);
  void flox_exec_step(FloxExecAlgoHandle handle, int64_t now_ns);
  void flox_exec_report_fill(FloxExecAlgoHandle handle, double qty);
  void flox_exec_observe_volume(FloxExecAlgoHandle handle, double qty);
  size_t flox_exec_pending_count(FloxExecAlgoHandle handle);
  uint8_t flox_exec_pending_at(FloxExecAlgoHandle handle, size_t index,
                               FloxExecChildOrder* out);
  void flox_exec_clear_pending(FloxExecAlgoHandle handle);
  double flox_exec_target_qty(FloxExecAlgoHandle handle);
  double flox_exec_submitted_qty(FloxExecAlgoHandle handle);
  double flox_exec_filled_qty(FloxExecAlgoHandle handle);
  double flox_exec_remaining_qty(FloxExecAlgoHandle handle);
  uint8_t flox_exec_is_done(FloxExecAlgoHandle handle);

  // ============================================================
  // Delta book compression (tape format)
  // ============================================================

  typedef void* FloxDeltaBookEncoderHandle;
  typedef void* FloxDeltaBookReplayerHandle;

  FloxDeltaBookEncoderHandle flox_delta_book_encoder_create(uint32_t anchor_every);
  void flox_delta_book_encoder_destroy(FloxDeltaBookEncoderHandle handle);
  void flox_delta_book_encoder_reset(FloxDeltaBookEncoderHandle handle, uint32_t symbol_id);
  void flox_delta_book_encoder_reset_all(FloxDeltaBookEncoderHandle handle);
  void flox_delta_book_encoder_encode(FloxDeltaBookEncoderHandle handle,
                                      uint32_t symbol_id,
                                      const FloxBookLevel* bids, size_t bid_count,
                                      const FloxBookLevel* asks, size_t ask_count,
                                      uint8_t* out_is_delta,
                                      uint64_t* out_bid_count,
                                      uint64_t* out_ask_count);
  uint64_t flox_delta_book_encoder_copy_bids(FloxDeltaBookEncoderHandle handle,
                                             FloxBookLevel* out, uint64_t max_entries);
  uint64_t flox_delta_book_encoder_copy_asks(FloxDeltaBookEncoderHandle handle,
                                             FloxBookLevel* out, uint64_t max_entries);

  FloxDeltaBookReplayerHandle flox_delta_book_replayer_create(void);
  void flox_delta_book_replayer_destroy(FloxDeltaBookReplayerHandle handle);
  void flox_delta_book_replayer_reset(FloxDeltaBookReplayerHandle handle, uint32_t symbol_id);
  void flox_delta_book_replayer_apply(FloxDeltaBookReplayerHandle handle,
                                      uint8_t type, uint32_t symbol_id,
                                      const FloxBookLevel* bids, size_t bid_count,
                                      const FloxBookLevel* asks, size_t ask_count,
                                      uint64_t* out_bid_count,
                                      uint64_t* out_ask_count);
  uint64_t flox_delta_book_replayer_copy_bids(FloxDeltaBookReplayerHandle handle,
                                              FloxBookLevel* out, uint64_t max_entries);
  uint64_t flox_delta_book_replayer_copy_asks(FloxDeltaBookReplayerHandle handle,
                                              FloxBookLevel* out, uint64_t max_entries);

  // ============================================================
  // Strategy run trace (.floxrun)
  // ============================================================
  typedef void* FloxRunRecorderHandle;
  typedef void* FloxRunReaderHandle;

  FloxRunRecorderHandle flox_run_recorder_create(const char* path,
                                                 const char* strategy_id,
                                                 const char* strategy_hash,
                                                 int64_t run_started_ns);
  void flox_run_recorder_destroy(FloxRunRecorderHandle handle);
  void flox_run_recorder_add_tape_ref(FloxRunRecorderHandle handle,
                                      const char* path,
                                      const char* content_hash,
                                      int64_t first_event_ns,
                                      int64_t last_event_ns);
  void flox_run_recorder_set_run_ended_ns(FloxRunRecorderHandle handle, int64_t ns);
  void flox_run_recorder_write_signal(FloxRunRecorderHandle handle,
                                      int64_t run_ts_ns, int64_t feed_ts_ns,
                                      uint32_t signal_id, uint32_t flags,
                                      int64_t strength_raw,
                                      const char* name, size_t name_len,
                                      const uint32_t* symbol_ids, size_t symbol_count,
                                      const uint8_t* payload, size_t payload_len);
  void flox_run_recorder_write_order_event(FloxRunRecorderHandle handle,
                                           int64_t run_ts_ns, int64_t feed_ts_ns,
                                           uint64_t order_id, uint64_t parent_signal_id,
                                           int64_t price_raw, int64_t qty_raw,
                                           uint32_t symbol_id, uint8_t event_kind,
                                           uint8_t side, uint8_t order_type,
                                           uint32_t flags,
                                           const char* reason, size_t reason_len);
  void flox_run_recorder_write_fill(FloxRunRecorderHandle handle,
                                    int64_t run_ts_ns, int64_t feed_ts_ns,
                                    uint64_t order_id, uint64_t fill_id,
                                    int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                                    uint32_t symbol_id, uint8_t side, uint8_t liquidity);
  void flox_run_recorder_close(FloxRunRecorderHandle handle);

  // ============================================================
  // Trace recorder auto-attach
  // ============================================================

  void flox_runner_attach_trace_recorder(FloxRunnerHandle runner,
                                         FloxRunRecorderHandle recorder);
  void flox_runner_set_trace_feed_ts_ns(FloxRunnerHandle runner, int64_t feed_ts_ns);
  void flox_runner_trace_order_event(FloxRunnerHandle runner, uint64_t order_id,
                                     uint64_t parent_signal_id, uint32_t symbol_id,
                                     uint8_t event_kind, uint8_t side, uint8_t order_type,
                                     int64_t price_raw, int64_t qty_raw, uint32_t flags);
  void flox_runner_trace_fill(FloxRunnerHandle runner, uint64_t order_id, uint64_t fill_id,
                              int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                              uint32_t symbol_id, uint8_t side, uint8_t liquidity);

  FloxRunReaderHandle flox_run_reader_open(const char* path);
  void flox_run_reader_close(FloxRunReaderHandle handle);
  uint64_t flox_run_reader_strategy_id(FloxRunReaderHandle handle, char* out, uint64_t max_bytes);
  uint64_t flox_run_reader_strategy_hash(FloxRunReaderHandle handle, char* out, uint64_t max_bytes);
  int64_t flox_run_reader_run_started_ns(FloxRunReaderHandle handle);
  int64_t flox_run_reader_run_ended_ns(FloxRunReaderHandle handle);
  uint64_t flox_run_reader_tape_ref_count(FloxRunReaderHandle handle);
  uint64_t flox_run_reader_tape_ref_path(FloxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes);
  uint64_t flox_run_reader_signal_count(FloxRunReaderHandle handle);
  uint64_t flox_run_reader_order_event_count(FloxRunReaderHandle handle);
  uint64_t flox_run_reader_fill_count(FloxRunReaderHandle handle);
  void flox_run_reader_signal_header(FloxRunReaderHandle handle, uint64_t index,
                                     int64_t* out_run_ts, int64_t* out_feed_ts,
                                     uint32_t* out_signal_id, uint32_t* out_flags,
                                     int64_t* out_strength_raw,
                                     uint64_t* out_name_len, uint64_t* out_symbol_count,
                                     uint64_t* out_payload_len);
  uint64_t flox_run_reader_signal_name(FloxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes);
  uint64_t flox_run_reader_signal_symbol_ids(FloxRunReaderHandle handle, uint64_t index, uint32_t* out, uint64_t max_entries);
  uint64_t flox_run_reader_signal_payload(FloxRunReaderHandle handle, uint64_t index, uint8_t* out, uint64_t max_bytes);
  void flox_run_reader_order_event_header(FloxRunReaderHandle handle, uint64_t index,
                                          int64_t* out_run_ts, int64_t* out_feed_ts,
                                          uint64_t* out_order_id, uint64_t* out_parent_signal_id,
                                          int64_t* out_price_raw, int64_t* out_qty_raw,
                                          uint32_t* out_symbol_id, uint8_t* out_event_kind,
                                          uint8_t* out_side, uint8_t* out_order_type,
                                          uint32_t* out_flags, uint64_t* out_reason_len);
  uint64_t flox_run_reader_order_event_reason(FloxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes);
  void flox_run_reader_fill(FloxRunReaderHandle handle, uint64_t index,
                            int64_t* out_run_ts, int64_t* out_feed_ts,
                            uint64_t* out_order_id, uint64_t* out_fill_id,
                            int64_t* out_price_raw, int64_t* out_qty_raw, int64_t* out_fee_raw,
                            uint32_t* out_symbol_id, uint8_t* out_side, uint8_t* out_liquidity);

  typedef void* FloxBarDispatchRecorderHandle;
  FloxBarDispatchRecorderHandle flox_bar_dispatch_recorder_create(void);
  void flox_bar_dispatch_recorder_destroy(FloxBarDispatchRecorderHandle h);
  uint32_t flox_bar_dispatch_recorder_add_time_seconds(FloxBarDispatchRecorderHandle h,
                                                       uint32_t seconds);
  void flox_bar_dispatch_recorder_on_trade(FloxBarDispatchRecorderHandle h, uint32_t symbol,
                                           double price, double qty, int64_t ts_ns);
  void flox_bar_dispatch_recorder_finalize(FloxBarDispatchRecorderHandle h);
  uint32_t flox_bar_dispatch_recorder_count(FloxBarDispatchRecorderHandle h);
  uint8_t flox_bar_dispatch_recorder_type_at(FloxBarDispatchRecorderHandle h, uint32_t index);
  uint64_t flox_bar_dispatch_recorder_param_at(FloxBarDispatchRecorderHandle h, uint32_t index);

#ifdef __cplusplus
}
#endif
