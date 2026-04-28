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
  typedef void* FloxExecutorHandle;
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
  typedef void (*FloxOnStartCallback)(void* user_data);
  typedef void (*FloxOnStopCallback)(void* user_data);

  typedef struct
  {
    FloxOnTradeCallback on_trade;
    FloxOnBookCallback on_book;
    FloxOnStartCallback on_start;
    FloxOnStopCallback on_stop;
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
  void flox_strategy_destroy(FloxStrategyHandle strategy);

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

  FloxExecutorHandle flox_executor_create(void);
  void flox_executor_destroy(FloxExecutorHandle executor);
  void flox_executor_submit_order(FloxExecutorHandle executor, uint64_t id, uint8_t side,
                                  double price, double quantity, uint8_t order_type,
                                  uint32_t symbol);
  void flox_executor_cancel_order(FloxExecutorHandle executor, uint64_t order_id);
  void flox_executor_cancel_all(FloxExecutorHandle executor, uint32_t symbol);
  void flox_executor_on_bar(FloxExecutorHandle executor, uint32_t symbol, double close_price);
  void flox_executor_on_trade(FloxExecutorHandle executor, uint32_t symbol, double price,
                              uint8_t is_buy);
  void flox_executor_advance_clock(FloxExecutorHandle executor, int64_t timestamp_ns);
  uint32_t flox_executor_fill_count(FloxExecutorHandle executor);

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

  uint32_t flox_executor_get_fills(FloxExecutorHandle executor, FloxFill* fills_out,
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
  void flox_executor_set_default_slippage(FloxExecutorHandle executor,
                                          int32_t model, int32_t ticks,
                                          double tick_size, double bps,
                                          double impact_coeff);
  void flox_executor_set_symbol_slippage(FloxExecutorHandle executor, uint32_t symbol,
                                         int32_t model, int32_t ticks,
                                         double tick_size, double bps,
                                         double impact_coeff);

  // Configure queue simulation for limit orders.
  void flox_executor_set_queue_model(FloxExecutorHandle executor, int32_t model,
                                     uint32_t depth);

  // Feed a trade with quantity (enables queue-fill simulation for limit orders).
  void flox_executor_on_trade_qty(FloxExecutorHandle executor, uint32_t symbol,
                                  double price, double quantity, uint8_t is_buy);

  // Feed a top-of-book snapshot (both best bid and best ask in one call).
  // For multi-level updates, build a BookUpdate on the C++ side; the C API
  // intentionally does not expose a stateful per-side helper because that
  // makes it too easy to accidentally clear the opposite side.
  void flox_executor_on_best_levels(FloxExecutorHandle executor, uint32_t symbol,
                                    double bid_price, double bid_qty, double ask_price,
                                    double ask_qty);

  // Feed a full L2 snapshot with parallel bid/ask arrays.
  void flox_executor_on_book_snapshot(FloxExecutorHandle executor, uint32_t symbol,
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
                                            FloxExecutorHandle executor);

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

  // Replay raw OHLCV arrays (timestamps in nanoseconds, close prices as double).
  // Returns 1 on success, 0 on error.
  int flox_backtest_runner_run_ohlcv(FloxBacktestRunnerHandle runner,
                                     const int64_t* timestamps_ns,
                                     const double* close_prices,
                                     uint32_t n,
                                     const char* symbol,
                                     FloxBacktestStats* stats_out);

#ifdef __cplusplus
}
#endif
