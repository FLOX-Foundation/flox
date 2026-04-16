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

#ifdef __cplusplus
}
#endif
