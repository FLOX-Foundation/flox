/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * GENERATED — do not edit by hand.
 * Source: include/flox/capi/flox_capi_spec.hpp
 * Tool:   tools/codegen/flox_codegen/emit_capi.py
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
  typedef void* FloxOrderJourneyTracerHandle;
  typedef void* FloxFootprintHandle;
  typedef void* FloxVolumeProfileHandle;
  typedef void* FloxMarketProfileHandle;
  typedef void* FloxCompositeBookHandle;
  typedef void* FloxIndicatorGraphHandle;
  typedef FloxIndicatorGraphHandle FloxStreamingGraphHandle;
  typedef void* FloxOrderGroupHandle;
  typedef void* FloxFeedClockHandle;
  typedef void* FloxL3BookHandle;
  typedef void* FloxDataWriterHandle;
  typedef void* FloxDataReaderHandle;
  typedef void* FloxLatencyDistributionHandle;
  typedef void* FloxRateLimitPolicyHandle;
  typedef void* FloxBacktestResultHandle;
  typedef void* FloxMergedTapeReaderHandle;
  typedef void* FloxPartitionerHandle;
  typedef void* FloxRiskManagerHandle;
  typedef void* FloxKillSwitchHandle;
  typedef void* FloxOrderValidatorHandle;
  typedef void* FloxPnLTrackerHandle;
  typedef void* FloxStorageSinkHandle;
  typedef void* FloxMarketDataRecorderHandle;
  typedef void* FloxBinaryLogRecorderHookHandle;
  typedef void* FloxReplaySourceHandle;
  typedef void* FloxExecutionListenerHandle;
  typedef void* FloxExecutorHandle;
  typedef void* FloxLiveEngineHandle;
  typedef void* FloxRunnerHandle;
  typedef void* FloxBacktestRunnerHandle;
  typedef void* FloxGridSearchHandle;
  typedef void* FloxLatencyModelHandle;
  typedef void* FloxTapeDiffHandle;
  typedef void* FloxPortfolioRiskHandle;
  typedef void* FloxExecAlgoHandle;
  typedef void* FloxDeltaBookEncoderHandle;
  typedef void* FloxDeltaBookReplayerHandle;
  typedef void* FloxRunRecorderHandle;
  typedef void* FloxRunReaderHandle;
  typedef void* FloxBarDispatchRecorderHandle;
  typedef void* FloxAggregatorHandle;
  typedef void* FloxFeeScheduleHandle;
  typedef void* FloxFundingScheduleHandle;
  typedef void* FloxLiveQueuePositionHandle;
  // ============================================================
  // Enums
  // ============================================================

  typedef enum
  {
    FLOX_SLIPPAGE_NONE = 0,
    FLOX_SLIPPAGE_FIXED_TICKS = 1,
    FLOX_SLIPPAGE_FIXED_BPS = 2,
    FLOX_SLIPPAGE_VOLUME_IMPACT = 3,
  } FloxSlippageModel;

  typedef enum
  {
    FLOX_QUEUE_NONE = 0,
    FLOX_QUEUE_TOB = 1,
    FLOX_QUEUE_FULL = 2,
  } FloxQueueModel;

  typedef enum
  {
    FLOX_AGG_FILTER_TRADES = 1,
    FLOX_AGG_FILTER_BOOKS_ONLY = 2,
    FLOX_AGG_FILTER_BOTH = 3,
  } FloxAggregatorEventFilter;

  // ============================================================
  // Flat event structs (C-compatible)
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
    int64_t price_raw;
    int64_t quantity_raw;
  } FloxBookLevel;

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

  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol_id;
    uint8_t side;
    uint8_t order_type;
    uint8_t status;
    uint8_t _pad;
    int64_t fill_qty_raw;
    int64_t fill_price_raw;
    int64_t exchange_ts_ns;
    const char* reject_reason;
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
    uint8_t is_maker;
    uint8_t market_position;
    int32_t distance_to_best_ticks;
    uint8_t _pad2[2];
  } FloxOrderEventData;

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

  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol;
    uint8_t side;
    int64_t price_raw;
    int64_t quantity_raw;
    int64_t timestamp_ns;
  } FloxFill;

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

  typedef struct
  {
    int64_t timestamp_ns;
    double equity;
    double drawdown_pct;
  } FloxEquityPoint;

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

  typedef struct
  {
    int64_t first_event_ns;
    int64_t last_event_ns;
    uint64_t total_events;
    uint32_t segment_count;
    uint64_t total_bytes;
    double duration_seconds;
  } FloxDatasetSummary;

  typedef struct
  {
    uint64_t files_read;
    uint64_t events_read;
    uint64_t trades_read;
    uint64_t book_updates_read;
    uint64_t bytes_read;
    uint64_t crc_errors;
  } FloxReaderStats;

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
    uint8_t event_type;
  } FloxBBO;

  typedef struct
  {
    int64_t exchange_ts_ns;
    int64_t recv_ts_ns;
    int64_t seq;
    uint64_t level_offset;
    uint32_t symbol_id;
    uint16_t bid_count;
    uint16_t ask_count;
    uint8_t event_type;
  } FloxBookUpdateHeader;

  typedef struct
  {
    int64_t price_raw;
    int64_t qty_raw;
    uint8_t side;
  } FloxLevel;

  typedef struct
  {
    uint32_t global_id;
    int8_t price_precision;
    int8_t qty_precision;
    uint8_t _pad[2];
    const char* exchange;
    const char* name;
  } FloxMergedSymbol;

  typedef struct
  {
    int64_t first_event_ns;
    int64_t last_event_ns;
    uint64_t trades;
    uint64_t books;
    const char* path;
  } FloxMergedTapeStats;

  typedef struct
  {
    uint64_t bytes_written;
    uint64_t events_written;
    uint64_t segments_created;
    uint64_t trades_written;
  } FloxWriterStats;

  typedef struct
  {
    uint32_t partition_id;
    int64_t from_ns;
    int64_t to_ns;
    int64_t warmup_from_ns;
    uint64_t estimated_events;
    uint64_t estimated_bytes;
  } FloxPartition;

  typedef struct
  {
    uint64_t order_id;
    uint32_t symbol;
    uint8_t side;
    uint8_t order_type;
    double price;
    double quantity;
    double trigger_price;
    double trailing_offset;
    int32_t trailing_bps;
    double new_price;
    double new_quantity;
  } FloxSignal;

  typedef struct
  {
    uint8_t type;
    uint8_t _pad[3];
    int64_t timestamp_ns;
    uint32_t trade_symbol;
    uint8_t trade_is_buy;
    uint8_t _pad2[3];
    int64_t trade_price_raw;
    int64_t trade_quantity_raw;
    uint32_t book_symbol;
    uint32_t n_bids;
    uint32_t n_asks;
    uint32_t _pad3;
    const FloxBookLevel* bids;
    const FloxBookLevel* asks;
  } FloxReplayEvent;

  typedef struct
  {
    uint64_t id;
    uint64_t client_order_id;
    uint32_t symbol;
    uint16_t strategy_id;
    uint16_t order_tag;
    uint8_t side;
    uint8_t type;
    uint8_t time_in_force;
    uint8_t flags;
    int64_t price_raw;
    int64_t quantity_raw;
    int64_t filled_quantity_raw;
    int64_t trigger_price_raw;
    int64_t trailing_offset_raw;
    int64_t created_at_ns;
    int64_t exchange_ts_ns;
  } FloxOrder;

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

  typedef struct
  {
    int64_t feed_ns;
    int64_t order_ns;
    int64_t fill_ns;
  } FloxLatencySample;

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

  typedef struct
  {
    uint64_t order_id;
    int64_t timestamp_ns;
    double qty;
    double price;
    uint8_t type;
  } FloxExecChildOrder;

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

  // ============================================================
  // Callback function pointer types
  // ============================================================

  typedef void (*FloxOnTradeCallback)(void*, const FloxSymbolContext*, const FloxTradeData*);
  typedef void (*FloxOnBookCallback)(void*, const FloxSymbolContext*, const FloxBookData*);
  typedef void (*FloxOnBarCallback)(void*, const FloxSymbolContext*, const FloxBarData*);
  typedef void (*FloxOnFillCallback)(void*, const FloxSymbolContext*, const FloxOrderEventData*);
  typedef void (*FloxOnOrderUpdateCallback)(void*, const FloxSymbolContext*, const FloxOrderEventData*);
  typedef void (*FloxOnStartCallback)(void*);
  typedef void (*FloxOnStopCallback)(void*);
  typedef void (*FloxOnQueuePositionChangeCallback)(void*, const FloxSymbolContext*, const FloxOrderEventData*);
  typedef void (*FloxOnMarketPositionChangeCallback)(void*, const FloxSymbolContext*, const FloxOrderEventData*);
  typedef const double* (*FloxGraphNodeFn)(void*, FloxIndicatorGraphHandle, uint32_t, size_t*);
  typedef void (*FloxOnSignalCallback)(void*, const FloxSignal*);
  typedef uint8_t (*FloxRiskManagerAllowFn)(void*, const FloxSignal*);
  typedef uint8_t (*FloxKillSwitchCheckFn)(void*, const FloxSignal*);
  typedef uint8_t (*FloxOrderValidatorValidateFn)(void*, const FloxSignal*);
  typedef void (*FloxLogCallback)(void*, int32_t, const char*);
  typedef void (*FloxPnLTrackerOnSignalFn)(void*, const FloxSignal*);
  typedef void (*FloxStorageSinkStoreFn)(void*, const FloxSignal*);
  typedef void (*FloxRecorderOnTradeFn)(void*, const FloxTradeData*);
  typedef void (*FloxRecorderOnBookUpdateFn)(void*, uint32_t, uint8_t, const FloxBookLevel*, uint32_t, const FloxBookLevel*, uint32_t, int64_t);
  typedef void (*FloxRecorderLifecycleFn)(void*);
  typedef uint8_t (*FloxReplaySourceNextFn)(void*, FloxReplayEvent*);
  typedef uint8_t (*FloxReplaySourceSeekFn)(void*, int64_t);
  typedef void (*FloxReplaySourceLifecycleFn)(void*);
  typedef void (*FloxExecListenerOnOrderFn)(void*, const FloxOrder*);
  typedef void (*FloxExecListenerOnPartialFillFn)(void*, const FloxOrder*, int64_t);
  typedef void (*FloxExecListenerOnRejectedFn)(void*, const FloxOrder*, const char*);
  typedef void (*FloxExecListenerOnReplacedFn)(void*, const FloxOrder*, const FloxOrder*);
  typedef void (*FloxExecListenerOnTrailingUpdateFn)(void*, const FloxOrder*, int64_t);
  typedef void (*FloxExecListenerOnQueuePositionChangeFn)(void*, const FloxOrder*, int64_t, int64_t);
  typedef void (*FloxExecListenerOnMarketPositionChangeFn)(void*, const FloxOrder*, uint8_t, int32_t);
  typedef void (*FloxExecListenerOnReplaceSubmittedFn)(void*, const FloxOrder*, const FloxOrder*);
  typedef void (*FloxExecListenerOnReplaceAcceptedFn)(void*, const FloxOrder*, const FloxOrder*);
  typedef void (*FloxExecListenerOnReplaceRejectedFn)(void*, const FloxOrder*, const FloxOrder*, const char*);
  typedef void (*FloxExecutorSubmitFn)(void*, const FloxOrder*);
  typedef void (*FloxExecutorCancelFn)(void*, uint64_t);
  typedef void (*FloxExecutorCancelAllFn)(void*, uint32_t);
  typedef void (*FloxExecutorReplaceFn)(void*, uint64_t, const FloxOrder*);
  typedef void (*FloxExecutorSubmitOCOFn)(void*, const FloxOrder*, const FloxOrder*);
  typedef void (*FloxExecutorCapabilitiesFn)(void*, FloxExchangeCapabilities*);
  typedef void (*FloxExecutorLifecycleFn)(void*);
  typedef FloxStrategyHandle (*FloxWalkForwardFactoryFn)(void*, uint64_t);
  typedef int (*FloxGridSearchFactoryFn)(void*, uint64_t, const double*, uint32_t, FloxBacktestStats*);
  typedef uint8_t (*FloxProgressCallback)(void*, double, int64_t);

  // ============================================================
  // Callback bundles
  // ============================================================

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

  typedef struct
  {
    FloxRiskManagerAllowFn allow;
    void* user_data;
  } FloxRiskManagerCallbacks;

  typedef struct
  {
    FloxKillSwitchCheckFn check;
    void* user_data;
  } FloxKillSwitchCallbacks;

  typedef struct
  {
    FloxOrderValidatorValidateFn validate;
    void* user_data;
  } FloxOrderValidatorCallbacks;

  typedef struct
  {
    FloxPnLTrackerOnSignalFn on_signal;
    void* user_data;
  } FloxPnLTrackerCallbacks;

  typedef struct
  {
    FloxStorageSinkStoreFn store;
    void* user_data;
  } FloxStorageSinkCallbacks;

  typedef struct
  {
    FloxRecorderOnTradeFn on_trade;
    FloxRecorderOnBookUpdateFn on_book_update;
    FloxRecorderLifecycleFn on_start;
    FloxRecorderLifecycleFn on_stop;
    void* user_data;
  } FloxMarketDataRecorderCallbacks;

  typedef struct
  {
    FloxReplaySourceLifecycleFn on_start;
    FloxReplaySourceLifecycleFn on_stop;
    FloxReplaySourceSeekFn seek_to;
    FloxReplaySourceNextFn next;
    void* user_data;
  } FloxReplaySourceCallbacks;

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

  // ============================================================
  // Fixed-point conversion helpers
  // ============================================================

  int64_t flox_price_from_double(double value);
  double flox_price_to_double(int64_t raw);
  int64_t flox_quantity_from_double(double value);
  double flox_quantity_to_double(int64_t raw);

  // ============================================================
  // Additional Bar
  // ============================================================

  uint32_t flox_aggregate_range_bars(const int64_t* timestamps, const double* prices,
                                     const double* quantities, const uint8_t* is_buy, size_t len,
                                     double range_size, FloxBar* bars_out, uint32_t max_bars);
  uint32_t flox_aggregate_renko_bars(const int64_t* timestamps, const double* prices,
                                     const double* quantities, const uint8_t* is_buy, size_t len,
                                     double brick_size, FloxBar* bars_out, uint32_t max_bars);

  // ============================================================
  // Additional Stats
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
  // Backtest Slippage
  // ============================================================

  void flox_simulated_executor_set_default_slippage(FloxSimulatedExecutorHandle executor,
                                                    int32_t model, int32_t ticks, double tick_size,
                                                    double bps, double impact_coeff);
  void flox_simulated_executor_set_symbol_slippage(FloxSimulatedExecutorHandle executor,
                                                   uint32_t symbol, int32_t model, int32_t ticks,
                                                   double tick_size, double bps, double impact_coeff);
  void flox_simulated_executor_set_queue_model(FloxSimulatedExecutorHandle executor, int32_t model,
                                               uint32_t depth);
  void flox_simulated_executor_set_submit_ack_latency(FloxSimulatedExecutorHandle executor,
                                                      int64_t latency_ns, int64_t jitter_ns);
  void flox_simulated_executor_set_cancel_ack_latency(FloxSimulatedExecutorHandle executor,
                                                      int64_t latency_ns, int64_t jitter_ns);
  void flox_simulated_executor_set_replace_ack_latency(FloxSimulatedExecutorHandle executor,
                                                       int64_t latency_ns, int64_t jitter_ns);
  void flox_simulated_executor_apply_latency_profile(FloxSimulatedExecutorHandle executor,
                                                     const char* profile_name);
  void flox_simulated_executor_set_stp_mode(FloxSimulatedExecutorHandle executor, uint8_t mode);
  FloxLatencyDistributionHandle flox_latency_distribution_create(void);
  void flox_latency_distribution_destroy(FloxLatencyDistributionHandle h);
  void flox_latency_distribution_set_constant(FloxLatencyDistributionHandle h, int64_t ns);
  void flox_latency_distribution_set_uniform(FloxLatencyDistributionHandle h, int64_t lo_ns,
                                             int64_t hi_ns);
  void flox_latency_distribution_set_lognormal(FloxLatencyDistributionHandle h, int64_t median_ns,
                                               double sigma);
  void flox_latency_distribution_set_empirical(FloxLatencyDistributionHandle h,
                                               const int64_t* samples_ns, uint32_t n_samples);
  void flox_latency_distribution_set_burst_correlation(FloxLatencyDistributionHandle h, double rho);
  int64_t flox_latency_distribution_median_ns(FloxLatencyDistributionHandle h);
  void flox_simulated_executor_set_submit_ack_latency_distribution(FloxSimulatedExecutorHandle executor,
                                                                   FloxLatencyDistributionHandle dist);
  void flox_simulated_executor_set_cancel_ack_latency_distribution(FloxSimulatedExecutorHandle executor,
                                                                   FloxLatencyDistributionHandle dist);
  void flox_simulated_executor_set_replace_ack_latency_distribution(FloxSimulatedExecutorHandle executor,
                                                                    FloxLatencyDistributionHandle dist);
  void flox_simulated_executor_on_trade_qty(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                            double price, double quantity, uint8_t is_buy);
  void flox_simulated_executor_on_best_levels(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                              double bid_price, double bid_qty, double ask_price,
                                              double ask_qty);
  void flox_simulated_executor_on_book_snapshot(FloxSimulatedExecutorHandle executor,
                                                uint32_t symbol, const double* bid_prices,
                                                const double* bid_qtys, uint32_t n_bids,
                                                const double* ask_prices, const double* ask_qtys,
                                                uint32_t n_asks);
  FloxBacktestResultHandle flox_backtest_result_create(double initial_capital, double fee_rate,
                                                       uint8_t use_percentage_fee,
                                                       double fixed_fee_per_trade,
                                                       double risk_free_rate,
                                                       double annualization_factor);
  void flox_backtest_result_destroy(FloxBacktestResultHandle result);
  void flox_backtest_result_record_fill(FloxBacktestResultHandle result, uint64_t order_id,
                                        uint32_t symbol, uint8_t side, double price, double quantity,
                                        int64_t timestamp_ns);
  void flox_backtest_result_ingest_executor(FloxBacktestResultHandle result,
                                            FloxSimulatedExecutorHandle executor);
  void flox_backtest_result_stats(FloxBacktestResultHandle result, FloxBacktestStats* out);
  uint32_t flox_backtest_result_equity_curve(FloxBacktestResultHandle result,
                                             FloxEquityPoint* points_out, uint32_t max_points);
  uint8_t flox_backtest_result_write_equity_curve_csv(FloxBacktestResultHandle result,
                                                      const char* path);
  uint32_t flox_backtest_result_trades(FloxBacktestResultHandle result,
                                       FloxBacktestTrade* trades_out, uint32_t max_trades);

  // ============================================================
  // Backtestrunner Replay
  // ============================================================

  FloxBacktestRunnerHandle flox_backtest_runner_create(FloxRegistryHandle registry, double fee_rate,
                                                       double initial_capital);
  void flox_backtest_runner_destroy(FloxBacktestRunnerHandle runner);
  void flox_backtest_runner_set_strategy(FloxBacktestRunnerHandle runner,
                                         FloxStrategyHandle strategy);
  int flox_backtest_runner_run_csv(FloxBacktestRunnerHandle runner, const char* path,
                                   const char* symbol, FloxBacktestStats* stats_out);
  int flox_backtest_runner_run_tape(FloxBacktestRunnerHandle runner, const char* tape_dir,
                                    FloxBacktestStats* stats_out);
  int flox_backtest_runner_run_tapes(FloxBacktestRunnerHandle runner, const char* const* tape_dirs,
                                     uint32_t n_dirs, FloxBacktestStats* stats_out);
  int flox_backtest_runner_run_ohlcv(FloxBacktestRunnerHandle runner, const int64_t* timestamps_ns,
                                     const double* close_prices, uint32_t n, const char* symbol,
                                     FloxBacktestStats* stats_out);
  int flox_backtest_runner_run_bars(FloxBacktestRunnerHandle runner, const int64_t* start_time_ns,
                                    const int64_t* end_time_ns, const double* open,
                                    const double* high, const double* low, const double* close,
                                    const double* volume, uint32_t n, const char* symbol,
                                    uint8_t bar_type, uint64_t bar_type_param,
                                    FloxBacktestStats* stats_out);
  int flox_backtest_runner_run_replay_source(FloxBacktestRunnerHandle runner,
                                             FloxReplaySourceHandle source,
                                             FloxBacktestStats* stats_out);
  FloxBacktestResultHandle flox_backtest_runner_take_result(FloxBacktestRunnerHandle runner);
  void flox_backtest_runner_set_risk_manager(FloxBacktestRunnerHandle runner,
                                             FloxRiskManagerHandle rm);
  void flox_backtest_runner_set_kill_switch(FloxBacktestRunnerHandle runner, FloxKillSwitchHandle ks);
  void flox_backtest_runner_set_order_validator(FloxBacktestRunnerHandle runner,
                                                FloxOrderValidatorHandle ov);
  void flox_backtest_runner_set_pnl_tracker(FloxBacktestRunnerHandle runner,
                                            FloxPnLTrackerHandle tracker);

  // ============================================================
  // Bar Aggregation
  // ============================================================

  uint32_t flox_aggregate_time_bars(const int64_t* timestamps, const double* prices,
                                    const double* quantities, const uint8_t* is_buy, size_t len,
                                    double interval_seconds, FloxBar* bars_out, uint32_t max_bars);
  uint32_t flox_aggregate_tick_bars(const int64_t* timestamps, const double* prices,
                                    const double* quantities, const uint8_t* is_buy, size_t len,
                                    uint32_t tick_count, FloxBar* bars_out, uint32_t max_bars);
  uint32_t flox_aggregate_volume_bars(const int64_t* timestamps, const double* prices,
                                      const double* quantities, const uint8_t* is_buy, size_t len,
                                      double volume_threshold, FloxBar* bars_out, uint32_t max_bars);

  // ============================================================
  // Bar Dispatch
  // ============================================================

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

  // ============================================================
  // Binary Log Recorder Hook
  // ============================================================

  FloxBinaryLogRecorderHookHandle flox_binary_log_recorder_hook_create(const char* output_dir,
                                                                       uint64_t max_segment_mb,
                                                                       uint8_t exchange_id,
                                                                       uint8_t compression);
  FloxBinaryLogRecorderHookHandle flox_binary_log_recorder_hook_create_ex(const char* output_dir,
                                                                          uint64_t max_segment_mb,
                                                                          uint8_t exchange_id,
                                                                          uint8_t compression,
                                                                          const char* exchange_name,
                                                                          const char* instrument_type);
  void flox_binary_log_recorder_hook_destroy(FloxBinaryLogRecorderHookHandle hook);
  FloxMarketDataRecorderHandle flox_binary_log_recorder_hook_as_recorder(FloxBinaryLogRecorderHookHandle hook);
  void flox_binary_log_recorder_hook_add_symbol(FloxBinaryLogRecorderHookHandle hook,
                                                uint32_t symbol_id, const char* name,
                                                const char* base, const char* quote,
                                                int8_t price_precision, int8_t qty_precision);
  void flox_binary_log_recorder_hook_flush(FloxBinaryLogRecorderHookHandle hook);
  FloxWriterStats flox_binary_log_recorder_hook_stats(FloxBinaryLogRecorderHookHandle hook);

  // ============================================================
  // Composite Book
  // ============================================================

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
  // Context Queries
  // ============================================================

  int64_t flox_position_raw(FloxStrategyHandle s, uint32_t symbol);
  int64_t flox_last_trade_price_raw(FloxStrategyHandle s, uint32_t symbol);
  int64_t flox_best_bid_raw(FloxStrategyHandle s, uint32_t symbol);
  int64_t flox_best_ask_raw(FloxStrategyHandle s, uint32_t symbol);
  int64_t flox_mid_price_raw(FloxStrategyHandle s, uint32_t symbol);
  void flox_get_symbol_context(FloxStrategyHandle s, uint32_t symbol, FloxSymbolContext* out);
  int32_t flox_get_order_status(FloxStrategyHandle s, uint64_t order_id);

  // ============================================================
  // Data Reader
  // ============================================================

  FloxDataReaderHandle flox_data_reader_create(const char* data_dir);
  void flox_data_reader_destroy(FloxDataReaderHandle reader);
  uint64_t flox_data_reader_count(FloxDataReaderHandle reader);
  void flox_data_reader_set_progress_callback(FloxDataReaderHandle reader, FloxProgressCallback cb,
                                              void* user_data, uint32_t interval_ms);
  void flox_data_reader_clear_progress_callback(FloxDataReaderHandle reader);

  // ============================================================
  // Data Writer
  // ============================================================

  FloxDataWriterHandle flox_data_writer_create(const char* output_dir, uint64_t max_segment_mb,
                                               uint8_t exchange_id);
  void flox_data_writer_destroy(FloxDataWriterHandle writer);
  uint8_t flox_data_writer_write_trade(FloxDataWriterHandle writer, int64_t exchange_ts_ns,
                                       int64_t recv_ts_ns, double price, double qty,
                                       uint64_t trade_id, uint32_t symbol_id, uint8_t side);
  uint8_t flox_data_writer_write_book(FloxDataWriterHandle writer, int64_t exchange_ts_ns,
                                      int64_t recv_ts_ns, int64_t seq, uint32_t symbol_id,
                                      uint8_t is_snapshot, const FloxBookLevel* bids,
                                      uint32_t n_bids, const FloxBookLevel* asks, uint32_t n_asks);
  void flox_data_writer_flush(FloxDataWriterHandle writer);
  void flox_data_writer_close(FloxDataWriterHandle writer);

  // ============================================================
  // Datareader
  // ============================================================

  FloxDataReaderHandle flox_data_reader_create_filtered(const char* data_dir, int64_t from_ns,
                                                        int64_t to_ns, const uint32_t* symbols,
                                                        uint32_t num_symbols);
  FloxDatasetSummary flox_data_reader_summary(FloxDataReaderHandle reader);
  FloxReaderStats flox_data_reader_stats(FloxDataReaderHandle reader);
  uint64_t flox_data_reader_read_trades(FloxDataReaderHandle reader, FloxTradeRecord* trades_out,
                                        uint64_t max_trades);
  uint64_t flox_data_reader_read_bbo(FloxDataReaderHandle reader, FloxBBO* bbos_out,
                                     uint64_t max_events);
  uint64_t flox_data_reader_count_book_updates(FloxDataReaderHandle reader,
                                               uint64_t* total_levels_out);
  uint64_t flox_data_reader_read_book_updates(FloxDataReaderHandle reader,
                                              FloxBookUpdateHeader* headers_out,
                                              uint64_t max_events, FloxLevel* levels_out,
                                              uint64_t max_levels);
  uint64_t flox_data_reader_read_trades_from(FloxDataReaderHandle reader, int64_t start_ts_ns,
                                             FloxTradeRecord* trades_out, uint64_t max_trades);
  uint64_t flox_data_reader_read_bbo_from(FloxDataReaderHandle reader, int64_t start_ts_ns,
                                          FloxBBO* bbos_out, uint64_t max_events);
  uint64_t flox_data_reader_count_book_updates_from(FloxDataReaderHandle reader, int64_t start_ts_ns,
                                                    uint64_t* total_levels_out);
  uint64_t flox_data_reader_read_book_updates_from(FloxDataReaderHandle reader, int64_t start_ts_ns,
                                                   FloxBookUpdateHeader* headers_out,
                                                   uint64_t max_events, FloxLevel* levels_out,
                                                   uint64_t max_levels);

  // ============================================================
  // Datawriter
  // ============================================================

  FloxWriterStats flox_data_writer_stats(FloxDataWriterHandle writer);
  uint64_t flox_data_writer_write_books(FloxDataWriterHandle writer,
                                        const FloxBookUpdateHeader* headers, uint64_t n_events,
                                        const FloxLevel* levels, uint64_t total_levels);

  // ============================================================
  // Delta Book
  // ============================================================

  FloxDeltaBookEncoderHandle flox_delta_book_encoder_create(uint32_t anchor_every);
  void flox_delta_book_encoder_destroy(FloxDeltaBookEncoderHandle handle);
  void flox_delta_book_encoder_reset(FloxDeltaBookEncoderHandle handle, uint32_t symbol_id);
  void flox_delta_book_encoder_reset_all(FloxDeltaBookEncoderHandle handle);
  void flox_delta_book_encoder_encode(FloxDeltaBookEncoderHandle handle, uint32_t symbol_id,
                                      const FloxBookLevel* bids, size_t bid_count,
                                      const FloxBookLevel* asks, size_t ask_count,
                                      uint8_t* out_is_delta, uint64_t* out_bid_count,
                                      uint64_t* out_ask_count);
  uint64_t flox_delta_book_encoder_copy_bids(FloxDeltaBookEncoderHandle handle, FloxBookLevel* out,
                                             uint64_t max_entries);
  uint64_t flox_delta_book_encoder_copy_asks(FloxDeltaBookEncoderHandle handle, FloxBookLevel* out,
                                             uint64_t max_entries);
  FloxDeltaBookReplayerHandle flox_delta_book_replayer_create(void);
  void flox_delta_book_replayer_destroy(FloxDeltaBookReplayerHandle handle);
  void flox_delta_book_replayer_reset(FloxDeltaBookReplayerHandle handle, uint32_t symbol_id);
  void flox_delta_book_replayer_apply(FloxDeltaBookReplayerHandle handle, uint8_t type,
                                      uint32_t symbol_id, const FloxBookLevel* bids,
                                      size_t bid_count, const FloxBookLevel* asks, size_t ask_count,
                                      uint64_t* out_bid_count, uint64_t* out_ask_count);
  uint64_t flox_delta_book_replayer_copy_bids(FloxDeltaBookReplayerHandle handle,
                                              FloxBookLevel* out, uint64_t max_entries);
  uint64_t flox_delta_book_replayer_copy_asks(FloxDeltaBookReplayerHandle handle,
                                              FloxBookLevel* out, uint64_t max_entries);

  // ============================================================
  // Execution
  // ============================================================

  FloxExecutionListenerHandle flox_execution_listener_create(FloxExecutionListenerCallbacks callbacks);
  FloxExecutionListenerHandle flox_execution_listener_create_p(const FloxExecutionListenerCallbacks* callbacks);
  void flox_execution_listener_destroy(FloxExecutionListenerHandle listener);
  FloxExecutorHandle flox_executor_create(FloxExecutorCallbacks callbacks);
  FloxExecutorHandle flox_executor_create_p(const FloxExecutorCallbacks* callbacks);
  void flox_executor_destroy(FloxExecutorHandle executor);
  void flox_executor_get_capabilities(FloxExecutorHandle executor,
                                      FloxExchangeCapabilities* caps_out);
  void flox_live_engine_set_executor(FloxLiveEngineHandle engine, FloxExecutorHandle executor);
  void flox_runner_set_executor(FloxRunnerHandle runner, FloxExecutorHandle executor);
  void flox_backtest_runner_add_execution_listener(FloxBacktestRunnerHandle runner,
                                                   FloxExecutionListenerHandle listener);
  void flox_backtest_runner_set_executor(FloxBacktestRunnerHandle runner,
                                         FloxExecutorHandle executor);

  // ============================================================
  // Execution Algos
  // ============================================================

  FloxExecAlgoHandle flox_exec_twap_create(double target_qty, uint8_t side, uint32_t symbol,
                                           uint8_t type, double limit_price, int64_t duration_ns,
                                           uint32_t slice_count, int64_t start_time_ns);
  FloxExecAlgoHandle flox_exec_vwap_create(double target_qty, uint8_t side, uint32_t symbol,
                                           uint8_t type, double limit_price,
                                           const int64_t* volume_curve_ts,
                                           const double* volume_curve_vol, size_t n);
  FloxExecAlgoHandle flox_exec_iceberg_create(double target_qty, uint8_t side, uint32_t symbol,
                                              uint8_t type, double limit_price, double visible_qty);
  FloxExecAlgoHandle flox_exec_pov_create(double target_qty, uint8_t side, uint32_t symbol,
                                          uint8_t type, double limit_price,
                                          double participation_rate, double min_slice_qty);
  void flox_exec_destroy(FloxExecAlgoHandle handle);
  void flox_exec_step(FloxExecAlgoHandle handle, int64_t now_ns);
  void flox_exec_report_fill(FloxExecAlgoHandle handle, double qty);
  void flox_exec_observe_volume(FloxExecAlgoHandle handle, double qty);
  size_t flox_exec_pending_count(FloxExecAlgoHandle handle);
  uint8_t flox_exec_pending_at(FloxExecAlgoHandle handle, size_t index, FloxExecChildOrder* out);
  void flox_exec_clear_pending(FloxExecAlgoHandle handle);
  double flox_exec_target_qty(FloxExecAlgoHandle handle);
  double flox_exec_submitted_qty(FloxExecAlgoHandle handle);
  double flox_exec_filled_qty(FloxExecAlgoHandle handle);
  double flox_exec_remaining_qty(FloxExecAlgoHandle handle);
  uint8_t flox_exec_is_done(FloxExecAlgoHandle handle);

  // ============================================================
  // Executor Fill
  // ============================================================

  uint32_t flox_simulated_executor_get_fills(FloxSimulatedExecutorHandle executor,
                                             FloxFill* fills_out, uint32_t max_fills);

  // ============================================================
  // Fee Schedule
  // ============================================================

  FloxFeeScheduleHandle flox_fee_schedule_create(void);
  void flox_fee_schedule_destroy(FloxFeeScheduleHandle h);
  void flox_fee_schedule_add_tier(FloxFeeScheduleHandle h, double min_notional_30d, double maker_bps,
                                  double taker_bps);
  void flox_fee_schedule_load_profile(FloxFeeScheduleHandle h, const char* profile_name);
  void flox_fee_schedule_record_fill(FloxFeeScheduleHandle h, int64_t ts_ns, double notional);
  double flox_fee_schedule_fee_for(FloxFeeScheduleHandle h, int64_t ts_ns, double notional,
                                   uint8_t is_maker);
  uint32_t flox_fee_schedule_current_tier(FloxFeeScheduleHandle h);
  double flox_fee_schedule_rolling_notional(FloxFeeScheduleHandle h);
  uint32_t flox_fee_schedule_tier_transitions(FloxFeeScheduleHandle h, int64_t* out_buf,
                                              uint32_t max_events);
  void flox_fee_schedule_reset_rolling(FloxFeeScheduleHandle h);

  // ============================================================
  // Feed Clock
  // ============================================================

  FloxFeedClockHandle flox_feed_clock_create(const uint32_t* symbols, uint32_t symbol_count,
                                             uint8_t policy, int64_t timeout_ms,
                                             uint32_t leader_symbol, int64_t staleness_budget_ms);
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
  // Floxliveengine Disruptor
  // ============================================================

  FloxLiveEngineHandle flox_live_engine_create(FloxRegistryHandle registry);
  void flox_live_engine_destroy(FloxLiveEngineHandle engine);
  void flox_live_engine_add_strategy(FloxLiveEngineHandle engine, FloxStrategyHandle strategy,
                                     FloxOnSignalCallback on_signal, void* user_data);
  void flox_live_engine_start(FloxLiveEngineHandle engine);
  void flox_live_engine_stop(FloxLiveEngineHandle engine);
  void flox_live_engine_publish_trade(FloxLiveEngineHandle engine, uint32_t symbol, double price,
                                      double qty, uint8_t is_buy, int64_t exchange_ts_ns);
  void flox_live_engine_publish_book_snapshot(FloxLiveEngineHandle engine, uint32_t symbol,
                                              const double* bid_prices, const double* bid_qtys,
                                              uint32_t n_bids, const double* ask_prices,
                                              const double* ask_qtys, uint32_t n_asks,
                                              int64_t exchange_ts_ns);
  void flox_live_engine_publish_bar(FloxLiveEngineHandle engine, uint32_t symbol, uint8_t bar_type,
                                    uint64_t bar_type_param, double open, double high, double low,
                                    double close, double volume, double buy_volume,
                                    int64_t start_time_ns, int64_t end_time_ns, uint8_t close_reason);

  // ============================================================
  // Floxrun
  // ============================================================

  FloxRunRecorderHandle flox_run_recorder_create(const char* path, const char* strategy_id,
                                                 const char* strategy_hash, int64_t run_started_ns);
  void flox_run_recorder_destroy(FloxRunRecorderHandle handle);
  void flox_run_recorder_add_tape_ref(FloxRunRecorderHandle handle, const char* path,
                                      const char* content_hash, int64_t first_event_ns,
                                      int64_t last_event_ns);
  void flox_run_recorder_set_run_ended_ns(FloxRunRecorderHandle handle, int64_t ns);
  void flox_run_recorder_write_signal(FloxRunRecorderHandle handle, int64_t run_ts_ns,
                                      int64_t feed_ts_ns, uint32_t signal_id, uint32_t flags,
                                      int64_t strength_raw, const char* name, size_t name_len,
                                      const uint32_t* symbol_ids, size_t symbol_count,
                                      const uint8_t* payload, size_t payload_len);
  void flox_run_recorder_write_order_event(FloxRunRecorderHandle handle, int64_t run_ts_ns,
                                           int64_t feed_ts_ns, uint64_t order_id,
                                           uint64_t parent_signal_id, int64_t price_raw,
                                           int64_t qty_raw, uint32_t symbol_id, uint8_t event_kind,
                                           uint8_t side, uint8_t order_type, uint32_t flags,
                                           const char* reason, size_t reason_len);
  void flox_run_recorder_write_fill(FloxRunRecorderHandle handle, int64_t run_ts_ns,
                                    int64_t feed_ts_ns, uint64_t order_id, uint64_t fill_id,
                                    int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                                    uint32_t symbol_id, uint8_t side, uint8_t liquidity);
  void flox_run_recorder_close(FloxRunRecorderHandle handle);
  FloxRunReaderHandle flox_run_reader_open(const char* path);
  void flox_run_reader_close(FloxRunReaderHandle handle);
  uint64_t flox_run_reader_strategy_id(FloxRunReaderHandle handle, char* out, uint64_t max_bytes);
  uint64_t flox_run_reader_strategy_hash(FloxRunReaderHandle handle, char* out, uint64_t max_bytes);
  int64_t flox_run_reader_run_started_ns(FloxRunReaderHandle handle);
  int64_t flox_run_reader_run_ended_ns(FloxRunReaderHandle handle);
  uint64_t flox_run_reader_tape_ref_count(FloxRunReaderHandle handle);
  uint64_t flox_run_reader_tape_ref_path(FloxRunReaderHandle handle, uint64_t index, char* out,
                                         uint64_t max_bytes);
  uint64_t flox_run_reader_signal_count(FloxRunReaderHandle handle);
  uint64_t flox_run_reader_order_event_count(FloxRunReaderHandle handle);
  uint64_t flox_run_reader_fill_count(FloxRunReaderHandle handle);
  void flox_run_reader_signal_header(FloxRunReaderHandle handle, uint64_t index,
                                     int64_t* out_run_ts, int64_t* out_feed_ts,
                                     uint32_t* out_signal_id, uint32_t* out_flags,
                                     int64_t* out_strength_raw, uint64_t* out_name_len,
                                     uint64_t* out_symbol_count, uint64_t* out_payload_len);
  uint64_t flox_run_reader_signal_name(FloxRunReaderHandle handle, uint64_t index, char* out,
                                       uint64_t max_bytes);
  uint64_t flox_run_reader_signal_symbol_ids(FloxRunReaderHandle handle, uint64_t index,
                                             uint32_t* out, uint64_t max_entries);
  uint64_t flox_run_reader_signal_payload(FloxRunReaderHandle handle, uint64_t index, uint8_t* out,
                                          uint64_t max_bytes);
  void flox_run_reader_order_event_header(FloxRunReaderHandle handle, uint64_t index,
                                          int64_t* out_run_ts, int64_t* out_feed_ts,
                                          uint64_t* out_order_id, uint64_t* out_parent_signal_id,
                                          int64_t* out_price_raw, int64_t* out_qty_raw,
                                          uint32_t* out_symbol_id, uint8_t* out_event_kind,
                                          uint8_t* out_side, uint8_t* out_order_type,
                                          uint32_t* out_flags, uint64_t* out_reason_len);
  uint64_t flox_run_reader_order_event_reason(FloxRunReaderHandle handle, uint64_t index, char* out,
                                              uint64_t max_bytes);
  void flox_run_reader_fill(FloxRunReaderHandle handle, uint64_t index, int64_t* out_run_ts,
                            int64_t* out_feed_ts, uint64_t* out_order_id, uint64_t* out_fill_id,
                            int64_t* out_price_raw, int64_t* out_qty_raw, int64_t* out_fee_raw,
                            uint32_t* out_symbol_id, uint8_t* out_side, uint8_t* out_liquidity);

  // ============================================================
  // Footprint Bar
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
  // Funding Schedule
  // ============================================================

  FloxFundingScheduleHandle flox_funding_schedule_create(void);
  void flox_funding_schedule_destroy(FloxFundingScheduleHandle h);
  void flox_funding_schedule_set_constant(FloxFundingScheduleHandle h, int64_t interval_ns,
                                          double rate);
  void flox_funding_schedule_set_tape(FloxFundingScheduleHandle h, const int64_t* timestamps_ns,
                                      const double* rates, uint32_t n_events);
  void flox_funding_schedule_load_profile(FloxFundingScheduleHandle h, const char* profile_name);
  void flox_funding_schedule_set_constant_rate(FloxFundingScheduleHandle h, double rate);
  void flox_funding_schedule_reset(FloxFundingScheduleHandle h);
  uint32_t flox_funding_schedule_tick(FloxFundingScheduleHandle h, int64_t now_ns,
                                      const uint32_t* symbols, const double* positions,
                                      const double* mark_prices, uint32_t n_symbols,
                                      double* out_buf, uint32_t max_events);

  // ============================================================
  // Grid Search
  // ============================================================

  FloxGridSearchHandle flox_grid_search_create(void);
  void flox_grid_search_destroy(FloxGridSearchHandle gs);
  void flox_grid_search_add_axis(FloxGridSearchHandle gs, const double* values, uint32_t num_values);
  uint64_t flox_grid_search_total(FloxGridSearchHandle gs);
  uint32_t flox_grid_search_params_for_index(FloxGridSearchHandle gs, uint64_t index,
                                             double* params_out, uint32_t max_params);
  uint64_t flox_grid_search_run(FloxGridSearchHandle gs, FloxGridSearchFactoryFn factory,
                                void* user_data, FloxBacktestStats* stats_out,
                                uint32_t max_results);

  // ============================================================
  // Heatmap
  // ============================================================

  uint64_t flox_render_heatmap_html(const FloxHeatmapData* data, char* out_buf, uint64_t max_size);

  // ============================================================
  // Heikin Ashi
  // ============================================================

  uint32_t flox_aggregate_heikin_ashi_bars(const int64_t* timestamps, const double* prices,
                                           const double* quantities, const uint8_t* is_buy,
                                           size_t len, double interval_seconds, FloxBar* bars_out,
                                           uint32_t max_bars);

  // ============================================================
  // Indicator Functions
  // ============================================================

  void flox_indicator_ema(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_sma(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_rsi(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_atr(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);
  void flox_indicator_macd(const double* input, size_t len, size_t fast_period, size_t slow_period,
                           size_t signal_period, double* macd_out, double* signal_out,
                           double* hist_out);
  void flox_indicator_bollinger(const double* input, size_t len, size_t period, double multiplier,
                                double* upper, double* middle, double* lower);
  void flox_indicator_rma(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_dema(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_tema(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_kama(const double* input, size_t len, size_t period, size_t fast, size_t slow,
                           double* output);
  void flox_indicator_slope(const double* input, size_t len, size_t length, double* output);
  void flox_indicator_adx(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* adx_out, double* plus_di_out,
                          double* minus_di_out);
  void flox_indicator_cci(const double* high, const double* low, const double* close, size_t len,
                          size_t period, double* output);
  void flox_indicator_stochastic(const double* high, const double* low, const double* close,
                                 size_t len, size_t k_period, size_t d_period, double* k_out,
                                 double* d_out);
  void flox_indicator_chop(const double* high, const double* low, const double* close, size_t len,
                           size_t period, double* output);
  void flox_indicator_obv(const double* close, const double* volume, size_t len, double* output);
  void flox_indicator_vwap(const double* close, const double* volume, size_t len, size_t window,
                           double* output);
  void flox_indicator_cvd(const double* open, const double* high, const double* low,
                          const double* close, const double* volume, size_t len, double* output);
  void flox_indicator_skewness(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_kurtosis(const double* input, size_t len, size_t period, double* output);
  void flox_indicator_parkinson_vol(const double* high, const double* low, size_t len,
                                    size_t period, double* output);
  void flox_indicator_rogers_satchell_vol(const double* open, const double* high,
                                          const double* low, const double* close, size_t len,
                                          size_t period, double* output);
  void flox_indicator_rolling_zscore(const double* input, size_t len, size_t period,
                                     double* output);
  void flox_indicator_shannon_entropy(const double* input, size_t len, size_t period, size_t bins,
                                      double* output);
  void flox_indicator_correlation(const double* x, const double* y, size_t len, size_t period,
                                  double* output);
  void flox_indicator_adf(const double* input, size_t len, size_t max_lag, const char* regression,
                          double* test_stat_out, double* p_value_out, size_t* used_lag_out);
  void flox_indicator_autocorrelation(const double* input, size_t len, size_t window, size_t lag,
                                      double* output);

  // ============================================================
  // Indicatorgraph
  // ============================================================

  FloxIndicatorGraphHandle flox_indicator_graph_create(void);
  void flox_indicator_graph_destroy(FloxIndicatorGraphHandle g);
  void flox_indicator_graph_set_bars(FloxIndicatorGraphHandle g, uint32_t symbol,
                                     const double* close, const double* high, const double* low,
                                     const double* volume, size_t len);
  void flox_indicator_graph_add_node(FloxIndicatorGraphHandle g, const char* name,
                                     const char* const* deps, size_t num_deps, FloxGraphNodeFn fn,
                                     void* user_data);
  const double* flox_indicator_graph_require(FloxIndicatorGraphHandle g, uint32_t symbol,
                                             const char* name, size_t* len_out);
  const double* flox_indicator_graph_get(FloxIndicatorGraphHandle g, uint32_t symbol,
                                         const char* name, size_t* len_out);
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
  void flox_indicator_graph_step(FloxIndicatorGraphHandle g, uint32_t symbol, double open,
                                 double high, double low, double close, double volume);
  double flox_indicator_graph_current(FloxIndicatorGraphHandle g, uint32_t symbol, const char* name);
  uint32_t flox_indicator_graph_bar_count(FloxIndicatorGraphHandle g, uint32_t symbol);
  void flox_indicator_graph_reset(FloxIndicatorGraphHandle g, uint32_t symbol);
  void flox_indicator_graph_reset_all(FloxIndicatorGraphHandle g);
  FloxStreamingGraphHandle flox_streaming_graph_create(void);
  void flox_streaming_graph_destroy(FloxStreamingGraphHandle sg);
  void flox_streaming_graph_add_node(FloxStreamingGraphHandle sg, const char* name,
                                     const char* const* deps, size_t num_deps, FloxGraphNodeFn fn,
                                     void* user_data);
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
  // L3 Order
  // ============================================================

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
  // Latency Models
  // ============================================================

  FloxLatencyModelHandle flox_latency_constant_create(int64_t feed_ns, int64_t order_ns,
                                                      int64_t fill_ns);
  FloxLatencyModelHandle flox_latency_gaussian_create(double feed_mean_ns, double feed_stddev_ns,
                                                      double order_mean_ns, double order_stddev_ns,
                                                      double fill_mean_ns, double fill_stddev_ns,
                                                      uint64_t seed);
  FloxLatencyModelHandle flox_latency_exponential_create(double feed_mean_ns, double order_mean_ns,
                                                         double fill_mean_ns, uint64_t seed);
  FloxLatencyModelHandle flox_latency_empirical_create(const int64_t* feed_samples,
                                                       size_t feed_count,
                                                       const int64_t* order_samples,
                                                       size_t order_count,
                                                       const int64_t* fill_samples,
                                                       size_t fill_count, uint64_t seed);
  void flox_latency_destroy(FloxLatencyModelHandle model);
  int64_t flox_latency_feed_delay(FloxLatencyModelHandle model);
  int64_t flox_latency_order_delay(FloxLatencyModelHandle model);
  int64_t flox_latency_fill_delay(FloxLatencyModelHandle model);
  void flox_latency_sample(FloxLatencyModelHandle model, FloxLatencySample* out);
  void flox_latency_reset(FloxLatencyModelHandle model, uint64_t seed);

  // ============================================================
  // Live Queue Position
  // ============================================================

  FloxLiveQueuePositionHandle flox_live_queue_position_create(void);
  void flox_live_queue_position_destroy(FloxLiveQueuePositionHandle h);
  void flox_live_queue_position_set_confidence_half_life_ns(FloxLiveQueuePositionHandle h,
                                                            int64_t half_life_ns);
  void flox_live_queue_position_set_shrink_factor(FloxLiveQueuePositionHandle h, double factor);
  void flox_live_queue_position_on_order_placed(FloxLiveQueuePositionHandle h, uint32_t symbol,
                                                uint8_t side, int64_t price_raw, uint64_t order_id,
                                                int64_t order_qty_raw, int64_t level_qty_raw,
                                                int64_t ts_ns);
  void flox_live_queue_position_on_order_cancelled(FloxLiveQueuePositionHandle h, uint64_t order_id,
                                                   int64_t ts_ns);
  void flox_live_queue_position_on_order_filled(FloxLiveQueuePositionHandle h, uint64_t order_id,
                                                int64_t cumulative_fill_raw, int64_t ts_ns);
  void flox_live_queue_position_on_trade(FloxLiveQueuePositionHandle h, uint32_t symbol,
                                         int64_t price_raw, int64_t qty_raw, int64_t ts_ns);
  void flox_live_queue_position_on_trade_with_flag(FloxLiveQueuePositionHandle h, uint32_t symbol,
                                                   int64_t price_raw, int64_t qty_raw, int64_t ts_ns,
                                                   uint8_t is_hidden);
  void flox_live_queue_position_set_hidden_order_policy(FloxLiveQueuePositionHandle h,
                                                        uint8_t policy);
  void flox_live_queue_position_on_level_update(FloxLiveQueuePositionHandle h, uint32_t symbol,
                                                uint8_t side, int64_t price_raw, int64_t new_qty_raw,
                                                int64_t ts_ns);
  uint8_t flox_live_queue_position_snapshot(FloxLiveQueuePositionHandle h, uint64_t order_id,
                                            int64_t now_ns, int64_t* out_slots);
  uint32_t flox_live_queue_position_tracked_count(FloxLiveQueuePositionHandle h);

  // ============================================================
  // Logger
  // ============================================================

  void flox_set_log_callback(FloxLogCallback callback, void* user_data);

  // ============================================================
  // Market Profile
  // ============================================================

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
  // Merged Tape Reader
  // ============================================================

  FloxMergedTapeReaderHandle flox_merged_tape_reader_create(const char* const* paths,
                                                            uint32_t n_paths, int64_t from_ns,
                                                            int64_t to_ns,
                                                            const uint32_t* symbol_filter,
                                                            uint32_t n_filter);
  void flox_merged_tape_reader_destroy(FloxMergedTapeReaderHandle reader);
  uint32_t flox_merged_tape_reader_symbol_count(FloxMergedTapeReaderHandle reader);
  uint32_t flox_merged_tape_reader_get_symbols(FloxMergedTapeReaderHandle reader,
                                               FloxMergedSymbol* out, uint32_t max);
  uint32_t flox_merged_tape_reader_tape_count(FloxMergedTapeReaderHandle reader);
  uint32_t flox_merged_tape_reader_get_tape_stats(FloxMergedTapeReaderHandle reader,
                                                  FloxMergedTapeStats* out, uint32_t max);
  void flox_merged_tape_reader_time_range(FloxMergedTapeReaderHandle reader,
                                          int64_t* min_first_ns_out, int64_t* max_last_ns_out);
  uint64_t flox_merged_tape_reader_count_trades(FloxMergedTapeReaderHandle reader);
  uint64_t flox_merged_tape_reader_read_trades(FloxMergedTapeReaderHandle reader,
                                               FloxTradeRecord* trades_out, uint64_t max_trades);
  uint64_t flox_merged_tape_reader_count_books(FloxMergedTapeReaderHandle reader,
                                               uint64_t* total_levels_out);
  uint64_t flox_merged_tape_reader_read_books(FloxMergedTapeReaderHandle reader,
                                              FloxBookUpdateHeader* headers_out,
                                              uint64_t max_events, FloxLevel* levels_out,
                                              uint64_t max_levels);

  // ============================================================
  // Metrics
  // ============================================================

  FloxPnLTrackerHandle flox_pnl_tracker_create(FloxPnLTrackerCallbacks callbacks);
  FloxPnLTrackerHandle flox_pnl_tracker_create_p(const FloxPnLTrackerCallbacks* callbacks);
  void flox_pnl_tracker_destroy(FloxPnLTrackerHandle tracker);
  void flox_live_engine_set_pnl_tracker(FloxLiveEngineHandle engine, FloxPnLTrackerHandle tracker);
  void flox_runner_set_pnl_tracker(FloxRunnerHandle runner, FloxPnLTrackerHandle tracker);

  // ============================================================
  // Multi Tf Helpers
  // ============================================================

  uint8_t flox_strategy_last_closed_bar(FloxStrategyHandle s, uint32_t symbol, uint8_t bar_type,
                                        uint64_t param, FloxBar* out);
  uint32_t flox_strategy_last_n_closed_bars(FloxStrategyHandle s, uint32_t symbol, uint8_t bar_type,
                                            uint64_t param, FloxBar* bars_out, uint32_t max_bars);
  uint32_t flox_strategy_get_bar_ring_capacity(FloxStrategyHandle s);
  void flox_strategy_set_bar_ring_capacity(FloxStrategyHandle s, uint32_t capacity);

  // ============================================================
  // Order Book
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
  uint32_t flox_book_get_bids(FloxBookHandle book, double* prices_out, double* qtys_out,
                              uint32_t max_levels);
  uint32_t flox_book_get_asks(FloxBookHandle book, double* prices_out, double* qtys_out,
                              uint32_t max_levels);

  // ============================================================
  // Order Group
  // ============================================================

  FloxOrderGroupHandle flox_order_group_create(uint64_t parent_signal_id, uint8_t policy);
  void flox_order_group_destroy(FloxOrderGroupHandle h);
  uint32_t flox_order_group_add_market_leg(FloxOrderGroupHandle h, uint32_t symbol, uint8_t side,
                                           int64_t qty_raw);
  uint32_t flox_order_group_add_limit_leg(FloxOrderGroupHandle h, uint32_t symbol, uint8_t side,
                                          int64_t price_raw, int64_t qty_raw);
  uint32_t flox_order_group_leg_count(FloxOrderGroupHandle h);
  uint8_t flox_order_group_leg_state(FloxOrderGroupHandle h, uint32_t leg_index);
  int64_t flox_order_group_leg_filled_raw(FloxOrderGroupHandle h, uint32_t leg_index);
  uint64_t flox_order_group_leg_order_id(FloxOrderGroupHandle h, uint32_t leg_index);
  void flox_order_group_record_submit(FloxOrderGroupHandle h, uint32_t leg_index, uint64_t order_id);
  void flox_order_group_record_fill(FloxOrderGroupHandle h, uint32_t leg_index,
                                    int64_t cumulative_qty_raw);
  void flox_order_group_record_cancel(FloxOrderGroupHandle h, uint32_t leg_index);
  void flox_order_group_record_failure(FloxOrderGroupHandle h, uint32_t leg_index);
  void flox_order_group_record_replace_accepted(FloxOrderGroupHandle h, uint32_t leg_index,
                                                uint64_t new_order_id);
  void flox_order_group_record_replace_rejected(FloxOrderGroupHandle h, uint32_t leg_index);
  uint32_t flox_order_group_find_leg_by_order_id(FloxOrderGroupHandle h, uint64_t order_id);
  uint8_t flox_order_group_state(FloxOrderGroupHandle h);
  uint32_t flox_order_group_recommended_actions(FloxOrderGroupHandle h, int64_t* actions_out,
                                                uint32_t max_actions);
  void flox_order_group_mark_action_dispatched(FloxOrderGroupHandle h, uint32_t leg_index,
                                               uint8_t kind);
  void flox_order_group_set_risk_limits(FloxOrderGroupHandle h, int64_t max_gross_notional_raw,
                                        double max_concentration_pct, int64_t max_leg_qty_raw);
  uint8_t flox_order_group_precheck_submission(FloxOrderGroupHandle h, double equity,
                                               const int64_t* market_ref_prices_raw,
                                               uint32_t market_ref_prices_len, char* rule_out,
                                               size_t rule_capacity, char* detail_out,
                                               size_t detail_capacity);
  void flox_order_group_set_pair_latency_budget_ns(FloxOrderGroupHandle h, int64_t budget_ns);
  uint8_t flox_order_group_pair_latency_decision(FloxOrderGroupHandle h, int64_t leader_submit_ts_ns,
                                                 int64_t leader_ack_ts_ns, uint8_t ack_received);

  // ============================================================
  // Order Journey Tracer
  // ============================================================

  FloxOrderJourneyTracerHandle flox_order_journey_tracer_create(uint64_t max_orders,
                                                                uint64_t max_records_per_order,
                                                                double sample_rate,
                                                                uint64_t sample_salt);
  void flox_order_journey_tracer_destroy(FloxOrderJourneyTracerHandle tracer);
  uint64_t flox_order_journey_tracer_order_count(FloxOrderJourneyTracerHandle tracer);
  uint64_t flox_order_journey_tracer_record_count(FloxOrderJourneyTracerHandle tracer);
  double flox_order_journey_tracer_median_ack_latency_ns(FloxOrderJourneyTracerHandle tracer);
  double flox_order_journey_tracer_median_time_to_first_fill_ns(FloxOrderJourneyTracerHandle tracer);
  double flox_order_journey_tracer_maker_fill_ratio(FloxOrderJourneyTracerHandle tracer);
  double flox_order_journey_tracer_cancel_race_loss_rate(FloxOrderJourneyTracerHandle tracer);
  uint64_t flox_order_journey_tracer_result(FloxOrderJourneyTracerHandle tracer,
                                            FloxOrderTraceRow* out, uint64_t max_rows);
  uint64_t flox_order_journey_tracer_journey(FloxOrderJourneyTracerHandle tracer, uint64_t order_id,
                                             FloxOrderTraceRow* out, uint64_t max_rows);
  void flox_order_journey_tracer_clear(FloxOrderJourneyTracerHandle tracer);
  void flox_backtest_runner_add_journey_tracer(FloxBacktestRunnerHandle runner,
                                               FloxOrderJourneyTracerHandle tracer);

  // ============================================================
  // Order Tracker
  // ============================================================

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
  // Partitioner
  // ============================================================

  FloxPartitionerHandle flox_partitioner_create(const char* data_dir);
  void flox_partitioner_destroy(FloxPartitionerHandle partitioner);
  uint32_t flox_partitioner_by_time(FloxPartitionerHandle p, uint32_t num_partitions,
                                    int64_t warmup_ns, FloxPartition* partitions_out,
                                    uint32_t max_partitions);
  uint32_t flox_partitioner_by_duration(FloxPartitionerHandle p, int64_t duration_ns,
                                        int64_t warmup_ns, FloxPartition* partitions_out,
                                        uint32_t max_partitions);
  uint32_t flox_partitioner_by_calendar(FloxPartitionerHandle p, uint8_t unit, int64_t warmup_ns,
                                        FloxPartition* partitions_out, uint32_t max_partitions);
  uint32_t flox_partitioner_by_symbol(FloxPartitionerHandle p, uint32_t num_partitions,
                                      FloxPartition* partitions_out, uint32_t max_partitions);
  uint32_t flox_partitioner_per_symbol(FloxPartitionerHandle p, FloxPartition* partitions_out,
                                       uint32_t max_partitions);
  uint32_t flox_partitioner_by_event_count(FloxPartitionerHandle p, uint32_t num_partitions,
                                           FloxPartition* partitions_out, uint32_t max_partitions);

  // ============================================================
  // Pointer Out
  // ============================================================

  void flox_data_reader_summary_p(FloxDataReaderHandle reader, void* out);
  void flox_data_reader_stats_p(FloxDataReaderHandle reader, void* out);
  void flox_data_writer_stats_p(FloxDataWriterHandle writer, void* out);
  void flox_binary_log_recorder_hook_stats_p(void* hook, void* out);
  void flox_segment_merge_full_p(const char* input_paths, size_t num_paths, const char* output_dir,
                                 const char* output_name, uint8_t sort, void* out);
  void flox_segment_merge_dir_p(const char* input_dir, const char* output_dir, void* out);
  void flox_segment_split_p(const char* input_path, const char* output_dir, uint8_t mode,
                            int64_t time_interval_ns, uint64_t events_per_file, void* out);
  void flox_segment_export_p(const char* input_path, const char* output_path, uint8_t format,
                             int64_t from_ns, int64_t to_ns, const uint32_t* symbols,
                             uint32_t num_symbols, void* out);
  void flox_segment_validate_full_p(const char* path, uint8_t verify_crc, uint8_t verify_timestamps,
                                    void* out);
  void flox_dataset_validate_p(const char* data_dir, void* out);

  // ============================================================
  // Portfolio Risk
  // ============================================================

  FloxPortfolioRiskHandle flox_portfolio_risk_create(const FloxPortfolioRiskRules* rules,
                                                     double initial_equity);
  void flox_portfolio_risk_destroy(FloxPortfolioRiskHandle handle);
  void flox_portfolio_risk_update(FloxPortfolioRiskHandle handle, const char* name,
                                  const FloxStrategyAccountFields* fields, uint8_t field_mask);
  void flox_portfolio_risk_remove(FloxPortfolioRiskHandle handle, const char* name);
  void flox_portfolio_risk_reset_kill_switch(FloxPortfolioRiskHandle handle);
  uint8_t flox_portfolio_risk_check_order(FloxPortfolioRiskHandle handle, const char* strategy,
                                          double notional, const char* side,
                                          FloxBreach* out_breach);
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
  // Position
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
  // Position Group
  // ============================================================

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
  // Rate Limit
  // ============================================================

  FloxRateLimitPolicyHandle flox_rate_limit_policy_create(void);
  void flox_rate_limit_policy_destroy(FloxRateLimitPolicyHandle h);
  void flox_rate_limit_policy_add_bucket(FloxRateLimitPolicyHandle h, const char* name,
                                         int64_t window_ns, uint32_t capacity,
                                         uint32_t submit_weight, uint32_t cancel_weight,
                                         uint32_t replace_weight);
  void flox_rate_limit_policy_set_ban(FloxRateLimitPolicyHandle h,
                                      uint32_t after_consecutive_rejects, int64_t ban_duration_ns);
  void flox_rate_limit_policy_load_profile(FloxRateLimitPolicyHandle h, const char* profile_name);
  int64_t flox_rate_limit_policy_ban_until_ns(FloxRateLimitPolicyHandle h);
  uint32_t flox_rate_limit_policy_consecutive_rejects(FloxRateLimitPolicyHandle h);
  uint32_t flox_rate_limit_policy_bucket_state(FloxRateLimitPolicyHandle h, int64_t now_ns,
                                               int64_t* out_buf, uint32_t max_buckets);
  void flox_simulated_executor_set_rate_limit_policy(FloxSimulatedExecutorHandle executor,
                                                     FloxRateLimitPolicyHandle policy);
  void flox_simulated_executor_clear_rate_limit_policy(FloxSimulatedExecutorHandle executor);

  // ============================================================
  // Recorder
  // ============================================================

  FloxMarketDataRecorderHandle flox_market_data_recorder_create(FloxMarketDataRecorderCallbacks callbacks);
  FloxMarketDataRecorderHandle flox_market_data_recorder_create_p(const FloxMarketDataRecorderCallbacks* callbacks);
  void flox_market_data_recorder_destroy(FloxMarketDataRecorderHandle recorder);
  void flox_live_engine_set_market_data_recorder(FloxLiveEngineHandle engine,
                                                 FloxMarketDataRecorderHandle recorder);
  void flox_runner_set_market_data_recorder(FloxRunnerHandle runner,
                                            FloxMarketDataRecorderHandle recorder);

  // ============================================================
  // Replay
  // ============================================================

  FloxReplaySourceHandle flox_replay_source_create(FloxReplaySourceCallbacks callbacks);
  FloxReplaySourceHandle flox_replay_source_create_p(const FloxReplaySourceCallbacks* callbacks);
  void flox_replay_source_destroy(FloxReplaySourceHandle source);
  uint8_t flox_replay_source_seek_to(FloxReplaySourceHandle source, int64_t timestamp_ns);

  // ============================================================
  // Risk
  // ============================================================

  FloxRiskManagerHandle flox_risk_manager_create(FloxRiskManagerCallbacks callbacks);
  FloxRiskManagerHandle flox_risk_manager_create_p(const FloxRiskManagerCallbacks* callbacks);
  void flox_risk_manager_destroy(FloxRiskManagerHandle rm);
  FloxKillSwitchHandle flox_kill_switch_create(FloxKillSwitchCallbacks callbacks);
  FloxKillSwitchHandle flox_kill_switch_create_p(const FloxKillSwitchCallbacks* callbacks);
  void flox_kill_switch_destroy(FloxKillSwitchHandle ks);
  FloxOrderValidatorHandle flox_order_validator_create(FloxOrderValidatorCallbacks callbacks);
  FloxOrderValidatorHandle flox_order_validator_create_p(const FloxOrderValidatorCallbacks* callbacks);
  void flox_order_validator_destroy(FloxOrderValidatorHandle ov);
  void flox_live_engine_set_risk_manager(FloxLiveEngineHandle engine, FloxRiskManagerHandle rm);
  void flox_live_engine_set_kill_switch(FloxLiveEngineHandle engine, FloxKillSwitchHandle ks);
  void flox_live_engine_set_order_validator(FloxLiveEngineHandle engine, FloxOrderValidatorHandle ov);
  void flox_runner_set_risk_manager(FloxRunnerHandle runner, FloxRiskManagerHandle rm);
  void flox_runner_set_kill_switch(FloxRunnerHandle runner, FloxKillSwitchHandle ks);
  void flox_runner_set_order_validator(FloxRunnerHandle runner, FloxOrderValidatorHandle ov);

  // ============================================================
  // Segment
  // ============================================================

  uint8_t flox_segment_validate(const char* path);
  uint8_t flox_segment_merge(const char* input_dir, const char* output_path);
  FloxMergeResult flox_segment_merge_full(const char* input_paths, size_t num_paths,
                                          const char* output_dir, const char* output_name,
                                          uint8_t sort);
  FloxMergeResult flox_segment_merge_dir(const char* input_dir, const char* output_dir);
  FloxSplitResult flox_segment_split(const char* input_path, const char* output_dir, uint8_t mode,
                                     int64_t time_interval_ns, uint64_t events_per_file);
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
  // Signal Emission
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
  // Simulated Executor
  // ============================================================

  FloxSimulatedExecutorHandle flox_simulated_executor_create(void);
  void flox_simulated_executor_destroy(FloxSimulatedExecutorHandle executor);
  void flox_simulated_executor_submit_order(FloxSimulatedExecutorHandle executor, uint64_t id,
                                            uint8_t side, double price, double quantity,
                                            uint8_t order_type, uint32_t symbol);
  void flox_simulated_executor_submit_order_ex(FloxSimulatedExecutorHandle executor, uint64_t id,
                                               uint8_t side, double price, double quantity,
                                               uint8_t order_type, uint32_t symbol, uint8_t tif,
                                               uint8_t reduce_only, int64_t expires_at_ns);
  void flox_simulated_executor_cancel_order(FloxSimulatedExecutorHandle executor, uint64_t order_id);
  void flox_simulated_executor_cancel_all(FloxSimulatedExecutorHandle executor, uint32_t symbol);
  void flox_simulated_executor_on_bar(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                      double close_price);
  void flox_simulated_executor_on_trade(FloxSimulatedExecutorHandle executor, uint32_t symbol,
                                        double price, uint8_t is_buy);
  void flox_simulated_executor_advance_clock(FloxSimulatedExecutorHandle executor,
                                             int64_t timestamp_ns);
  uint32_t flox_simulated_executor_fill_count(FloxSimulatedExecutorHandle executor);

  // ============================================================
  // Statistics
  // ============================================================

  double flox_stat_correlation(const double* x, const double* y, size_t len);
  double flox_stat_profit_factor(const double* pnl, size_t len);
  double flox_stat_win_rate(const double* pnl, size_t len);

  // ============================================================
  // Storage
  // ============================================================

  FloxStorageSinkHandle flox_storage_sink_create(FloxStorageSinkCallbacks callbacks);
  FloxStorageSinkHandle flox_storage_sink_create_p(const FloxStorageSinkCallbacks* callbacks);
  void flox_storage_sink_destroy(FloxStorageSinkHandle sink);
  void flox_live_engine_set_storage_sink(FloxLiveEngineHandle engine, FloxStorageSinkHandle sink);
  void flox_runner_set_storage_sink(FloxRunnerHandle runner, FloxStorageSinkHandle sink);

  // ============================================================
  // Strategy Lifecycle
  // ============================================================

  FloxStrategyHandle flox_strategy_create(uint32_t id, const uint32_t* symbols,
                                          uint32_t num_symbols, FloxRegistryHandle registry,
                                          FloxStrategyCallbacks callbacks);
  void flox_strategy_destroy(FloxStrategyHandle strategy);
  void flox_strategy_replace_callbacks(FloxStrategyHandle strategy, FloxStrategyCallbacks callbacks);
  FloxStrategyHandle flox_strategy_create_p(uint32_t id, const uint32_t* symbols,
                                            uint32_t num_symbols, FloxRegistryHandle registry,
                                            const FloxStrategyCallbacks* callbacks);
  void flox_strategy_replace_callbacks_p(FloxStrategyHandle strategy,
                                         const FloxStrategyCallbacks* callbacks);

  // ============================================================
  // Strategyrunner Synchronous
  // ============================================================

  FloxRunnerHandle flox_runner_create(FloxRegistryHandle registry, FloxOnSignalCallback on_signal,
                                      void* user_data);
  void flox_runner_destroy(FloxRunnerHandle runner);
  void flox_runner_add_strategy(FloxRunnerHandle runner, FloxStrategyHandle strategy);
  void flox_runner_start(FloxRunnerHandle runner);
  void flox_runner_stop(FloxRunnerHandle runner);
  void flox_runner_on_trade(FloxRunnerHandle runner, uint32_t symbol, double price, double qty,
                            uint8_t is_buy, int64_t exchange_ts_ns);
  void flox_runner_on_book_snapshot(FloxRunnerHandle runner, uint32_t symbol,
                                    const double* bid_prices, const double* bid_qtys,
                                    uint32_t n_bids, const double* ask_prices,
                                    const double* ask_qtys, uint32_t n_asks, int64_t exchange_ts_ns);
  void flox_runner_on_bar(FloxRunnerHandle runner, uint32_t symbol, uint8_t bar_type,
                          uint64_t bar_type_param, double open, double high, double low,
                          double close, double volume, double buy_volume, int64_t start_time_ns,
                          int64_t end_time_ns, uint8_t close_reason);

  // ============================================================
  // Symbol Registry
  // ============================================================

  FloxRegistryHandle flox_registry_create(void);
  void flox_registry_destroy(FloxRegistryHandle registry);
  uint32_t flox_registry_add_symbol(FloxRegistryHandle registry, const char* exchange,
                                    const char* name, double tick_size);
  uint8_t flox_registry_get_symbol_id(FloxRegistryHandle registry, const char* exchange,
                                      const char* name, uint32_t* id_out);
  uint8_t flox_registry_get_symbol_name(FloxRegistryHandle registry, uint32_t symbol_id,
                                        char* exchange_out, size_t exchange_len, char* name_out,
                                        size_t name_len);
  uint32_t flox_registry_symbol_count(FloxRegistryHandle registry);

  // ============================================================
  // Tape Aggregator
  // ============================================================

  FloxAggregatorHandle flox_event_type_stats_aggregator_create(FloxAggregatorEventFilter event_filter,
                                                               const uint32_t* symbol_filter,
                                                               uint32_t symbol_filter_count);
  FloxAggregatorHandle flox_bin_count_aggregator_create(int64_t bucket_ns, uint8_t by_side,
                                                        uint8_t by_symbol,
                                                        FloxAggregatorEventFilter event_filter,
                                                        const uint32_t* symbol_filter,
                                                        uint32_t symbol_filter_count);
  FloxAggregatorHandle flox_volume_bin_aggregator_create(int64_t bucket_ns, uint8_t by_side,
                                                         uint8_t by_symbol,
                                                         FloxAggregatorEventFilter event_filter,
                                                         const uint32_t* symbol_filter,
                                                         uint32_t symbol_filter_count);
  FloxAggregatorHandle flox_ohlc_bin_aggregator_create(int64_t bucket_ns, uint8_t by_symbol,
                                                       FloxAggregatorEventFilter event_filter,
                                                       const uint32_t* symbol_filter,
                                                       uint32_t symbol_filter_count);
  FloxAggregatorHandle flox_peak_aggregator_create(const int64_t* window_ns_list,
                                                   uint32_t window_count, uint32_t top_n,
                                                   uint32_t oversample_factor,
                                                   FloxAggregatorEventFilter event_filter,
                                                   const uint32_t* symbol_filter,
                                                   uint32_t symbol_filter_count);
  FloxAggregatorHandle flox_quantile_aggregator_create(const int64_t* window_ns_list,
                                                       uint32_t window_count,
                                                       const double* quantiles,
                                                       uint32_t quantile_count,
                                                       FloxAggregatorEventFilter event_filter,
                                                       const uint32_t* symbol_filter,
                                                       uint32_t symbol_filter_count);
  void flox_aggregator_destroy(FloxAggregatorHandle h);
  uint8_t flox_data_reader_run(FloxDataReaderHandle reader, FloxAggregatorHandle* aggregators,
                               uint32_t aggregator_count, uint32_t n_threads);
  uint8_t flox_merged_tape_reader_run(FloxMergedTapeReaderHandle reader,
                                      FloxAggregatorHandle* aggregators, uint32_t aggregator_count,
                                      uint32_t n_threads);
  uint32_t flox_event_type_stats_read_result(FloxAggregatorHandle h,
                                             FloxEventTypeStatsRow* rows_out, uint32_t max_rows);
  uint32_t flox_bin_count_read_result(FloxAggregatorHandle h, FloxBinCountRow* rows_out,
                                      uint32_t max_rows);
  uint32_t flox_volume_bin_read_result(FloxAggregatorHandle h, FloxVolumeBinRow* rows_out,
                                       uint32_t max_rows);
  uint32_t flox_ohlc_bin_read_result(FloxAggregatorHandle h, FloxOHLCBinRow* rows_out,
                                     uint32_t max_rows);
  uint32_t flox_peak_read_result(FloxAggregatorHandle h, FloxPeakRow* rows_out, uint32_t max_rows);
  uint32_t flox_quantile_read_result(FloxAggregatorHandle h, FloxQuantileRow* rows_out,
                                     uint32_t max_rows);

  // ============================================================
  // Tape Diff
  // ============================================================

  FloxTapeDiffHandle flox_tape_diff_create(const char* left_path, const char* right_path,
                                           uint32_t max_mismatches, int64_t field_tolerance_ns);
  void flox_tape_diff_destroy(FloxTapeDiffHandle handle);
  uint64_t flox_tape_diff_left_count(FloxTapeDiffHandle handle);
  uint64_t flox_tape_diff_right_count(FloxTapeDiffHandle handle);
  uint8_t flox_tape_diff_first_divergence(FloxTapeDiffHandle handle, uint64_t* out_index);
  uint8_t flox_tape_diff_equal(FloxTapeDiffHandle handle);
  uint64_t flox_tape_diff_mismatch_count(FloxTapeDiffHandle handle);
  uint64_t flox_tape_diff_copy_mismatches(FloxTapeDiffHandle handle, FloxTapeDiffMismatch* out,
                                          uint64_t max_entries);

  // ============================================================
  // Targets
  // ============================================================

  void flox_target_future_return(const double* close, size_t len, size_t horizon, double* output);
  void flox_target_future_ctc_volatility(const double* close, size_t len, size_t horizon,
                                         double* output);
  void flox_target_future_linear_slope(const double* close, size_t len, size_t horizon,
                                       double* output);

  // ============================================================
  // Trace Attach
  // ============================================================

  void flox_runner_attach_trace_recorder(FloxRunnerHandle runner, FloxRunRecorderHandle recorder);
  void flox_runner_set_trace_feed_ts_ns(FloxRunnerHandle runner, int64_t feed_ts_ns);
  void flox_runner_trace_order_event(FloxRunnerHandle runner, uint64_t order_id,
                                     uint64_t parent_signal_id, uint32_t symbol_id,
                                     uint8_t event_kind, uint8_t side, uint8_t order_type,
                                     int64_t price_raw, int64_t qty_raw, uint32_t flags);
  void flox_runner_trace_fill(FloxRunnerHandle runner, uint64_t order_id, uint64_t fill_id,
                              int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                              uint32_t symbol_id, uint8_t side, uint8_t liquidity);

  // ============================================================
  // Validation
  // ============================================================

  FloxSegmentValidation flox_segment_validate_full(const char* path, uint8_t verify_crc,
                                                   uint8_t verify_timestamps);
  FloxDatasetValidation flox_dataset_validate(const char* data_dir);

  // ============================================================
  // Volume Profile
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
  // Walk Forward
  // ============================================================

  uint32_t flox_walk_forward_run_csv(FloxRegistryHandle registry, const char* csv_path,
                                     const char* symbol, double fee_rate, double initial_capital,
                                     const FloxWalkForwardConfig* cfg,
                                     FloxWalkForwardFactoryFn factory, void* user_data,
                                     FloxWalkForwardFold* folds_out, uint32_t max_folds);

#ifdef __cplusplus
}
#endif
