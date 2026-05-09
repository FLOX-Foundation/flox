# FLOX C API Reference

Generated from `include/flox/capi/flox_capi_spec.hpp`. Source of truth for FFI consumers (Codon, QuickJS, Rust, Go cgo, Python ctypes). The pybind11 (Python) and NAPI (Node) bindings wrap this surface but expose richer language-native APIs that live in `python/` and `node/` respectively — see those for the Python/TS-flavored interfaces.

**Surface:** 460 functions, 42 handles, 48 structs, 33 callback typedefs, 2 enums, 57 groups.

## Opaque handles

All handles are typedef'd `void*`. Treat them as opaque; manage lifetime via the matching `_create` / `_destroy` pair.

- `FloxStrategyHandle`
- `FloxRegistryHandle`
- `FloxBookHandle`
- `FloxSimulatedExecutorHandle`
- `FloxPositionTrackerHandle`
- `FloxPositionGroupHandle`
- `FloxOrderTrackerHandle`
- `FloxFootprintHandle`
- `FloxVolumeProfileHandle`
- `FloxMarketProfileHandle`
- `FloxCompositeBookHandle`
- `FloxIndicatorGraphHandle`
- `FloxStreamingGraphHandle` (alias of `FloxIndicatorGraphHandle`)
- `FloxOrderGroupHandle`
- `FloxFeedClockHandle`
- `FloxL3BookHandle`
- `FloxDataWriterHandle`
- `FloxDataReaderHandle`
- `FloxBacktestResultHandle`
- `FloxDataRecorderHandle`
- `FloxPartitionerHandle`
- `FloxRiskManagerHandle`
- `FloxKillSwitchHandle`
- `FloxOrderValidatorHandle`
- `FloxPnLTrackerHandle`
- `FloxStorageSinkHandle`
- `FloxMarketDataRecorderHandle`
- `FloxReplaySourceHandle`
- `FloxExecutionListenerHandle`
- `FloxExecutorHandle`
- `FloxLiveEngineHandle`
- `FloxRunnerHandle`
- `FloxBacktestRunnerHandle`
- `FloxGridSearchHandle`
- `FloxLatencyModelHandle`
- `FloxTapeDiffHandle`
- `FloxPortfolioRiskHandle`
- `FloxExecAlgoHandle`
- `FloxDeltaBookEncoderHandle`
- `FloxDeltaBookReplayerHandle`
- `FloxRunRecorderHandle`
- `FloxRunReaderHandle`

## Enums

### `FloxSlippageModel`

- `FLOX_SLIPPAGE_NONE` = `0`
- `FLOX_SLIPPAGE_FIXED_TICKS` = `1`
- `FLOX_SLIPPAGE_FIXED_BPS` = `2`
- `FLOX_SLIPPAGE_VOLUME_IMPACT` = `3`

### `FloxQueueModel`

- `FLOX_QUEUE_NONE` = `0`
- `FLOX_QUEUE_TOB` = `1`
- `FLOX_QUEUE_FULL` = `2`

## Callback typedefs

- `typedef void (*FloxOnTradeCallback)(void *, const FloxSymbolContext *, const FloxTradeData *);`
- `typedef void (*FloxOnBookCallback)(void *, const FloxSymbolContext *, const FloxBookData *);`
- `typedef void (*FloxOnBarCallback)(void *, const FloxSymbolContext *, const FloxBarData *);`
- `typedef void (*FloxOnStartCallback)(void *);`
- `typedef void (*FloxOnStopCallback)(void *);`
- `typedef const double * (*FloxGraphNodeFn)(void *, FloxIndicatorGraphHandle, uint32_t, size_t *);`
- `typedef void (*FloxOnSignalCallback)(void *, const FloxSignal *);`
- `typedef uint8_t (*FloxRiskManagerAllowFn)(void *, const FloxSignal *);`
- `typedef uint8_t (*FloxKillSwitchCheckFn)(void *, const FloxSignal *);`
- `typedef uint8_t (*FloxOrderValidatorValidateFn)(void *, const FloxSignal *);`
- `typedef void (*FloxLogCallback)(void *, int32_t, const char *);`
- `typedef void (*FloxPnLTrackerOnSignalFn)(void *, const FloxSignal *);`
- `typedef void (*FloxStorageSinkStoreFn)(void *, const FloxSignal *);`
- `typedef void (*FloxRecorderOnTradeFn)(void *, const FloxTradeData *);`
- `typedef void (*FloxRecorderOnBookUpdateFn)(void *, uint32_t, uint8_t, const FloxBookLevel *, uint32_t, const FloxBookLevel *, uint32_t, int64_t);`
- `typedef void (*FloxRecorderLifecycleFn)(void *);`
- `typedef uint8_t (*FloxReplaySourceNextFn)(void *, FloxReplayEvent *);`
- `typedef uint8_t (*FloxReplaySourceSeekFn)(void *, int64_t);`
- `typedef void (*FloxReplaySourceLifecycleFn)(void *);`
- `typedef void (*FloxExecListenerOnOrderFn)(void *, const FloxOrder *);`
- `typedef void (*FloxExecListenerOnPartialFillFn)(void *, const FloxOrder *, int64_t);`
- `typedef void (*FloxExecListenerOnRejectedFn)(void *, const FloxOrder *, const char *);`
- `typedef void (*FloxExecListenerOnReplacedFn)(void *, const FloxOrder *, const FloxOrder *);`
- `typedef void (*FloxExecListenerOnTrailingUpdateFn)(void *, const FloxOrder *, int64_t);`
- `typedef void (*FloxExecutorSubmitFn)(void *, const FloxOrder *);`
- `typedef void (*FloxExecutorCancelFn)(void *, uint64_t);`
- `typedef void (*FloxExecutorCancelAllFn)(void *, uint32_t);`
- `typedef void (*FloxExecutorReplaceFn)(void *, uint64_t, const FloxOrder *);`
- `typedef void (*FloxExecutorSubmitOCOFn)(void *, const FloxOrder *, const FloxOrder *);`
- `typedef void (*FloxExecutorCapabilitiesFn)(void *, FloxExchangeCapabilities *);`
- `typedef void (*FloxExecutorLifecycleFn)(void *);`
- `typedef FloxStrategyHandle (*FloxWalkForwardFactoryFn)(void *, uint64_t);`
- `typedef int (*FloxGridSearchFactoryFn)(void *, uint64_t, const double *, uint32_t, FloxBacktestStats *);`

## Structs

### `FloxTradeData`

| field | type |
|---|---|
| `symbol` | `uint32_t` |
| `price_raw` | `int64_t` |
| `quantity_raw` | `int64_t` |
| `is_buy` | `uint8_t` |
| `exchange_ts_ns` | `int64_t` |

### `FloxBookLevel`

| field | type |
|---|---|
| `price_raw` | `int64_t` |
| `quantity_raw` | `int64_t` |

### `FloxBookSnapshot`

| field | type |
|---|---|
| `bid_price_raw` | `int64_t` |
| `bid_qty_raw` | `int64_t` |
| `ask_price_raw` | `int64_t` |
| `ask_qty_raw` | `int64_t` |
| `mid_raw` | `int64_t` |
| `spread_raw` | `int64_t` |

### `FloxBookData`

| field | type |
|---|---|
| `symbol` | `uint32_t` |
| `exchange_ts_ns` | `int64_t` |
| `snapshot` | `FloxBookSnapshot` |

### `FloxBarData`

| field | type |
|---|---|
| `symbol` | `uint32_t` |
| `bar_type` | `uint8_t` |
| `close_reason` | `uint8_t` |
| `_pad` | `uint8_t[2]` |
| `bar_type_param` | `uint64_t` |
| `open_raw` | `int64_t` |
| `high_raw` | `int64_t` |
| `low_raw` | `int64_t` |
| `close_raw` | `int64_t` |
| `volume_raw` | `int64_t` |
| `buy_volume_raw` | `int64_t` |
| `trade_count_raw` | `int64_t` |
| `start_time_ns` | `int64_t` |
| `end_time_ns` | `int64_t` |

### `FloxSymbolContext`

| field | type |
|---|---|
| `symbol_id` | `uint32_t` |
| `position_raw` | `int64_t` |
| `avg_entry_price_raw` | `int64_t` |
| `last_trade_price_raw` | `int64_t` |
| `last_update_ns` | `int64_t` |
| `book` | `FloxBookSnapshot` |

### `FloxStrategyCallbacks`

| field | type |
|---|---|
| `on_trade` | `FloxOnTradeCallback` |
| `on_book` | `FloxOnBookCallback` |
| `on_bar` | `FloxOnBarCallback` |
| `on_start` | `FloxOnStartCallback` |
| `on_stop` | `FloxOnStopCallback` |
| `user_data` | `void *` |

### `FloxBar`

| field | type |
|---|---|
| `start_time_ns` | `int64_t` |
| `end_time_ns` | `int64_t` |
| `open_raw` | `int64_t` |
| `high_raw` | `int64_t` |
| `low_raw` | `int64_t` |
| `close_raw` | `int64_t` |
| `volume_raw` | `int64_t` |
| `buy_volume_raw` | `int64_t` |
| `trade_count` | `uint32_t` |

### `FloxFill`

| field | type |
|---|---|
| `order_id` | `uint64_t` |
| `symbol` | `uint32_t` |
| `side` | `uint8_t` |
| `price_raw` | `int64_t` |
| `quantity_raw` | `int64_t` |
| `timestamp_ns` | `int64_t` |

### `FloxBacktestStats`

| field | type |
|---|---|
| `totalTrades` | `uint64_t` |
| `winningTrades` | `uint64_t` |
| `losingTrades` | `uint64_t` |
| `maxConsecutiveWins` | `uint64_t` |
| `maxConsecutiveLosses` | `uint64_t` |
| `initialCapital` | `double` |
| `finalCapital` | `double` |
| `totalPnl` | `double` |
| `totalFees` | `double` |
| `netPnl` | `double` |
| `grossProfit` | `double` |
| `grossLoss` | `double` |
| `maxDrawdown` | `double` |
| `maxDrawdownPct` | `double` |
| `winRate` | `double` |
| `profitFactor` | `double` |
| `avgWin` | `double` |
| `avgLoss` | `double` |
| `avgWinLossRatio` | `double` |
| `avgTradeDurationNs` | `double` |
| `medianTradeDurationNs` | `double` |
| `maxTradeDurationNs` | `double` |
| `sharpeRatio` | `double` |
| `sortinoRatio` | `double` |
| `calmarRatio` | `double` |
| `timeWeightedReturn` | `double` |
| `returnPct` | `double` |
| `startTimeNs` | `int64_t` |
| `endTimeNs` | `int64_t` |

### `FloxEquityPoint`

| field | type |
|---|---|
| `timestamp_ns` | `int64_t` |
| `equity` | `double` |
| `drawdown_pct` | `double` |

### `FloxBacktestTrade`

| field | type |
|---|---|
| `symbol` | `uint32_t` |
| `side` | `uint8_t` |
| `entry_price` | `double` |
| `exit_price` | `double` |
| `quantity` | `double` |
| `entry_time_ns` | `int64_t` |
| `exit_time_ns` | `int64_t` |
| `pnl` | `double` |
| `fee` | `double` |

### `FloxMergeResult`

| field | type |
|---|---|
| `success` | `uint8_t` |
| `segments_merged` | `uint64_t` |
| `events_written` | `uint64_t` |
| `bytes_written` | `uint64_t` |

### `FloxSplitResult`

| field | type |
|---|---|
| `success` | `uint8_t` |
| `segments_created` | `uint32_t` |
| `events_written` | `uint64_t` |

### `FloxExportResult`

| field | type |
|---|---|
| `success` | `uint8_t` |
| `events_exported` | `uint64_t` |
| `bytes_written` | `uint64_t` |

### `FloxSegmentValidation`

| field | type |
|---|---|
| `valid` | `uint8_t` |
| `header_valid` | `uint8_t` |
| `reported_event_count` | `uint64_t` |
| `actual_event_count` | `uint64_t` |
| `has_index` | `uint8_t` |
| `index_valid` | `uint8_t` |
| `trades_found` | `uint64_t` |
| `book_updates_found` | `uint64_t` |
| `crc_errors` | `uint32_t` |
| `timestamp_anomalies` | `uint32_t` |

### `FloxDatasetValidation`

| field | type |
|---|---|
| `valid` | `uint8_t` |
| `total_segments` | `uint32_t` |
| `valid_segments` | `uint32_t` |
| `corrupted_segments` | `uint32_t` |
| `total_events` | `uint64_t` |
| `total_bytes` | `uint64_t` |
| `first_timestamp` | `int64_t` |
| `last_timestamp` | `int64_t` |

### `FloxDatasetSummary`

| field | type |
|---|---|
| `first_event_ns` | `int64_t` |
| `last_event_ns` | `int64_t` |
| `total_events` | `uint64_t` |
| `segment_count` | `uint32_t` |
| `total_bytes` | `uint64_t` |
| `duration_seconds` | `double` |

### `FloxReaderStats`

| field | type |
|---|---|
| `files_read` | `uint64_t` |
| `events_read` | `uint64_t` |
| `trades_read` | `uint64_t` |
| `book_updates_read` | `uint64_t` |
| `bytes_read` | `uint64_t` |
| `crc_errors` | `uint64_t` |

### `FloxTradeRecord`

| field | type |
|---|---|
| `exchange_ts_ns` | `int64_t` |
| `recv_ts_ns` | `int64_t` |
| `price_raw` | `int64_t` |
| `qty_raw` | `int64_t` |
| `trade_id` | `uint64_t` |
| `symbol_id` | `uint32_t` |
| `side` | `uint8_t` |

### `FloxBBO`

| field | type |
|---|---|
| `exchange_ts_ns` | `int64_t` |
| `recv_ts_ns` | `int64_t` |
| `seq` | `int64_t` |
| `bid_price_raw` | `int64_t` |
| `bid_qty_raw` | `int64_t` |
| `ask_price_raw` | `int64_t` |
| `ask_qty_raw` | `int64_t` |
| `symbol_id` | `uint32_t` |
| `event_type` | `uint8_t` |

### `FloxBookUpdateHeader`

| field | type |
|---|---|
| `exchange_ts_ns` | `int64_t` |
| `recv_ts_ns` | `int64_t` |
| `seq` | `int64_t` |
| `level_offset` | `uint64_t` |
| `symbol_id` | `uint32_t` |
| `bid_count` | `uint16_t` |
| `ask_count` | `uint16_t` |
| `event_type` | `uint8_t` |

### `FloxLevel`

| field | type |
|---|---|
| `price_raw` | `int64_t` |
| `qty_raw` | `int64_t` |
| `side` | `uint8_t` |

### `FloxWriterStats`

| field | type |
|---|---|
| `bytes_written` | `uint64_t` |
| `events_written` | `uint64_t` |
| `segments_created` | `uint64_t` |
| `trades_written` | `uint64_t` |

### `FloxPartition`

| field | type |
|---|---|
| `partition_id` | `uint32_t` |
| `from_ns` | `int64_t` |
| `to_ns` | `int64_t` |
| `warmup_from_ns` | `int64_t` |
| `estimated_events` | `uint64_t` |
| `estimated_bytes` | `uint64_t` |

### `FloxSignal`

| field | type |
|---|---|
| `order_id` | `uint64_t` |
| `symbol` | `uint32_t` |
| `side` | `uint8_t` |
| `order_type` | `uint8_t` |
| `price` | `double` |
| `quantity` | `double` |
| `trigger_price` | `double` |
| `trailing_offset` | `double` |
| `trailing_bps` | `int32_t` |
| `new_price` | `double` |
| `new_quantity` | `double` |

### `FloxRiskManagerCallbacks`

| field | type |
|---|---|
| `allow` | `FloxRiskManagerAllowFn` |
| `user_data` | `void *` |

### `FloxKillSwitchCallbacks`

| field | type |
|---|---|
| `check` | `FloxKillSwitchCheckFn` |
| `user_data` | `void *` |

### `FloxOrderValidatorCallbacks`

| field | type |
|---|---|
| `validate` | `FloxOrderValidatorValidateFn` |
| `user_data` | `void *` |

### `FloxPnLTrackerCallbacks`

| field | type |
|---|---|
| `on_signal` | `FloxPnLTrackerOnSignalFn` |
| `user_data` | `void *` |

### `FloxStorageSinkCallbacks`

| field | type |
|---|---|
| `store` | `FloxStorageSinkStoreFn` |
| `user_data` | `void *` |

### `FloxMarketDataRecorderCallbacks`

| field | type |
|---|---|
| `on_trade` | `FloxRecorderOnTradeFn` |
| `on_book_update` | `FloxRecorderOnBookUpdateFn` |
| `on_start` | `FloxRecorderLifecycleFn` |
| `on_stop` | `FloxRecorderLifecycleFn` |
| `user_data` | `void *` |

### `FloxReplayEvent`

| field | type |
|---|---|
| `type` | `uint8_t` |
| `_pad` | `uint8_t[3]` |
| `timestamp_ns` | `int64_t` |
| `trade_symbol` | `uint32_t` |
| `trade_is_buy` | `uint8_t` |
| `_pad2` | `uint8_t[3]` |
| `trade_price_raw` | `int64_t` |
| `trade_quantity_raw` | `int64_t` |
| `book_symbol` | `uint32_t` |
| `n_bids` | `uint32_t` |
| `n_asks` | `uint32_t` |
| `_pad3` | `uint32_t` |
| `bids` | `const FloxBookLevel *` |
| `asks` | `const FloxBookLevel *` |

### `FloxReplaySourceCallbacks`

| field | type |
|---|---|
| `on_start` | `FloxReplaySourceLifecycleFn` |
| `on_stop` | `FloxReplaySourceLifecycleFn` |
| `seek_to` | `FloxReplaySourceSeekFn` |
| `next` | `FloxReplaySourceNextFn` |
| `user_data` | `void *` |

### `FloxOrder`

| field | type |
|---|---|
| `id` | `uint64_t` |
| `client_order_id` | `uint64_t` |
| `symbol` | `uint32_t` |
| `strategy_id` | `uint16_t` |
| `order_tag` | `uint16_t` |
| `side` | `uint8_t` |
| `type` | `uint8_t` |
| `time_in_force` | `uint8_t` |
| `flags` | `uint8_t` |
| `price_raw` | `int64_t` |
| `quantity_raw` | `int64_t` |
| `filled_quantity_raw` | `int64_t` |
| `trigger_price_raw` | `int64_t` |
| `trailing_offset_raw` | `int64_t` |
| `created_at_ns` | `int64_t` |
| `exchange_ts_ns` | `int64_t` |

### `FloxExecutionListenerCallbacks`

| field | type |
|---|---|
| `on_submitted` | `FloxExecListenerOnOrderFn` |
| `on_accepted` | `FloxExecListenerOnOrderFn` |
| `on_partially_filled` | `FloxExecListenerOnPartialFillFn` |
| `on_filled` | `FloxExecListenerOnOrderFn` |
| `on_pending_cancel` | `FloxExecListenerOnOrderFn` |
| `on_canceled` | `FloxExecListenerOnOrderFn` |
| `on_expired` | `FloxExecListenerOnOrderFn` |
| `on_rejected` | `FloxExecListenerOnRejectedFn` |
| `on_replaced` | `FloxExecListenerOnReplacedFn` |
| `on_pending_trigger` | `FloxExecListenerOnOrderFn` |
| `on_triggered` | `FloxExecListenerOnOrderFn` |
| `on_trailing_stop_updated` | `FloxExecListenerOnTrailingUpdateFn` |
| `user_data` | `void *` |

### `FloxExchangeCapabilities`

| field | type |
|---|---|
| `supports_stop_market` | `uint8_t` |
| `supports_stop_limit` | `uint8_t` |
| `supports_take_profit_market` | `uint8_t` |
| `supports_take_profit_limit` | `uint8_t` |
| `supports_trailing_stop` | `uint8_t` |
| `supports_iceberg` | `uint8_t` |
| `supports_oco` | `uint8_t` |
| `supports_gtc` | `uint8_t` |
| `supports_ioc` | `uint8_t` |
| `supports_fok` | `uint8_t` |
| `supports_gtd` | `uint8_t` |
| `supports_post_only` | `uint8_t` |
| `supports_reduce_only` | `uint8_t` |
| `supports_close_position` | `uint8_t` |
| `_pad` | `uint8_t[2]` |

### `FloxExecutorCallbacks`

| field | type |
|---|---|
| `submit` | `FloxExecutorSubmitFn` |
| `cancel` | `FloxExecutorCancelFn` |
| `cancel_all` | `FloxExecutorCancelAllFn` |
| `replace` | `FloxExecutorReplaceFn` |
| `submit_oco` | `FloxExecutorSubmitOCOFn` |
| `capabilities` | `FloxExecutorCapabilitiesFn` |
| `on_start` | `FloxExecutorLifecycleFn` |
| `on_stop` | `FloxExecutorLifecycleFn` |
| `user_data` | `void *` |

### `FloxWalkForwardConfig`

| field | type |
|---|---|
| `mode` | `uint8_t` |
| `train_size` | `uint64_t` |
| `test_size` | `uint64_t` |
| `step` | `uint64_t` |
| `min_train_size` | `uint64_t` |

### `FloxWalkForwardFold`

| field | type |
|---|---|
| `fold_index` | `uint64_t` |
| `train_start_bar` | `uint64_t` |
| `train_end_bar` | `uint64_t` |
| `test_start_bar` | `uint64_t` |
| `test_end_bar` | `uint64_t` |
| `train_start_ns` | `int64_t` |
| `train_end_ns` | `int64_t` |
| `test_start_ns` | `int64_t` |
| `test_end_ns` | `int64_t` |
| `train_stats` | `FloxBacktestStats` |
| `test_stats` | `FloxBacktestStats` |

### `FloxHeatmapData`

| field | type |
|---|---|
| `z` | `const double *` |
| `rows` | `uint32_t` |
| `cols` | `uint32_t` |
| `row_labels` | `const char *const *` |
| `num_row_labels` | `uint32_t` |
| `col_labels` | `const char *const *` |
| `num_col_labels` | `uint32_t` |
| `title` | `const char *` |
| `x_axis_name` | `const char *` |
| `y_axis_name` | `const char *` |
| `metric_name` | `const char *` |

### `FloxLatencySample`

| field | type |
|---|---|
| `feed_ns` | `int64_t` |
| `order_ns` | `int64_t` |
| `fill_ns` | `int64_t` |

### `FloxTapeDiffTrade`

| field | type |
|---|---|
| `exchange_ts_ns` | `int64_t` |
| `price_raw` | `int64_t` |
| `qty_raw` | `int64_t` |
| `symbol_id` | `uint32_t` |
| `side` | `uint8_t` |

### `FloxTapeDiffMismatch`

| field | type |
|---|---|
| `index` | `uint64_t` |
| `left` | `FloxTapeDiffTrade` |
| `right` | `FloxTapeDiffTrade` |

### `FloxPortfolioRiskRules`

| field | type |
|---|---|
| `has_max_drawdown_pct` | `uint8_t` |
| `max_drawdown_pct` | `double` |
| `has_max_daily_loss` | `uint8_t` |
| `max_daily_loss` | `double` |
| `has_max_gross_exposure` | `uint8_t` |
| `max_gross_exposure` | `double` |
| `has_max_concentration_pct` | `uint8_t` |
| `max_concentration_pct` | `double` |

### `FloxStrategyAccountFields`

| field | type |
|---|---|
| `realized_pnl` | `double` |
| `unrealized_pnl` | `double` |
| `fees` | `double` |
| `gross_exposure` | `double` |
| `net_exposure` | `double` |
| `trade_count` | `uint64_t` |

### `FloxBreach`

| field | type |
|---|---|
| `rule` | `const char *` |
| `value` | `double` |
| `limit` | `double` |
| `detail` | `const char *` |

### `FloxExecChildOrder`

| field | type |
|---|---|
| `order_id` | `uint64_t` |
| `timestamp_ns` | `int64_t` |
| `qty` | `double` |
| `price` | `double` |
| `type` | `uint8_t` |

## Functions

### additional_bar

- `uint32_t flox_aggregate_range_bars(const int64_t * timestamps, const double * prices, const double * quantities, const uint8_t * is_buy, size_t len, double range_size, FloxBar * bars_out, uint32_t max_bars)`
- `uint32_t flox_aggregate_renko_bars(const int64_t * timestamps, const double * prices, const double * quantities, const uint8_t * is_buy, size_t len, double brick_size, FloxBar * bars_out, uint32_t max_bars)`

### additional_stats

- `double flox_stat_permutation_test(const double * group1, size_t len1, const double * group2, size_t len2, uint32_t num_permutations)`
- `void flox_stat_bootstrap_ci(const double * data, size_t len, double confidence, uint32_t num_samples, double * lower_out, double * median_out, double * upper_out)`
- `void flox_stat_whites_reality_check(const double * returns, size_t num_strategies, size_t num_periods, uint32_t num_bootstrap, double avg_block_size, double * p_value_out, double * best_stat_out, int32_t * best_index_out)`

### backtest_slippage

- `void flox_simulated_executor_set_default_slippage(FloxSimulatedExecutorHandle executor, int32_t model, int32_t ticks, double tick_size, double bps, double impact_coeff)`
- `void flox_simulated_executor_set_symbol_slippage(FloxSimulatedExecutorHandle executor, uint32_t symbol, int32_t model, int32_t ticks, double tick_size, double bps, double impact_coeff)`
- `void flox_simulated_executor_set_queue_model(FloxSimulatedExecutorHandle executor, int32_t model, uint32_t depth)`
- `void flox_simulated_executor_on_trade_qty(FloxSimulatedExecutorHandle executor, uint32_t symbol, double price, double quantity, uint8_t is_buy)`
- `void flox_simulated_executor_on_best_levels(FloxSimulatedExecutorHandle executor, uint32_t symbol, double bid_price, double bid_qty, double ask_price, double ask_qty)`
- `void flox_simulated_executor_on_book_snapshot(FloxSimulatedExecutorHandle executor, uint32_t symbol, const double * bid_prices, const double * bid_qtys, uint32_t n_bids, const double * ask_prices, const double * ask_qtys, uint32_t n_asks)`
- `FloxBacktestResultHandle flox_backtest_result_create(double initial_capital, double fee_rate, uint8_t use_percentage_fee, double fixed_fee_per_trade, double risk_free_rate, double annualization_factor)`
- `void flox_backtest_result_destroy(FloxBacktestResultHandle result)`
- `void flox_backtest_result_record_fill(FloxBacktestResultHandle result, uint64_t order_id, uint32_t symbol, uint8_t side, double price, double quantity, int64_t timestamp_ns)`
- `void flox_backtest_result_ingest_executor(FloxBacktestResultHandle result, FloxSimulatedExecutorHandle executor)`
- `void flox_backtest_result_stats(FloxBacktestResultHandle result, FloxBacktestStats * out)`
- `uint32_t flox_backtest_result_equity_curve(FloxBacktestResultHandle result, FloxEquityPoint * points_out, uint32_t max_points)`
- `uint8_t flox_backtest_result_write_equity_curve_csv(FloxBacktestResultHandle result, const char * path)`
- `uint32_t flox_backtest_result_trades(FloxBacktestResultHandle result, FloxBacktestTrade * trades_out, uint32_t max_trades)`

### backtestrunner_replay

- `FloxBacktestRunnerHandle flox_backtest_runner_create(FloxRegistryHandle registry, double fee_rate, double initial_capital)`
- `void flox_backtest_runner_destroy(FloxBacktestRunnerHandle runner)`
- `void flox_backtest_runner_set_strategy(FloxBacktestRunnerHandle runner, FloxStrategyHandle strategy)`
- `int flox_backtest_runner_run_csv(FloxBacktestRunnerHandle runner, const char * path, const char * symbol, FloxBacktestStats * stats_out)`
- `int flox_backtest_runner_run_ohlcv(FloxBacktestRunnerHandle runner, const int64_t * timestamps_ns, const double * close_prices, uint32_t n, const char * symbol, FloxBacktestStats * stats_out)`
- `int flox_backtest_runner_run_bars(FloxBacktestRunnerHandle runner, const int64_t * start_time_ns, const int64_t * end_time_ns, const double * open, const double * high, const double * low, const double * close, const double * volume, uint32_t n, const char * symbol, uint8_t bar_type, uint64_t bar_type_param, FloxBacktestStats * stats_out)`
- `int flox_backtest_runner_run_replay_source(FloxBacktestRunnerHandle runner, FloxReplaySourceHandle source, FloxBacktestStats * stats_out)`
- `FloxBacktestResultHandle flox_backtest_runner_take_result(FloxBacktestRunnerHandle runner)`

### bar_aggregation

- `uint32_t flox_aggregate_time_bars(const int64_t * timestamps, const double * prices, const double * quantities, const uint8_t * is_buy, size_t len, double interval_seconds, FloxBar * bars_out, uint32_t max_bars)`
- `uint32_t flox_aggregate_tick_bars(const int64_t * timestamps, const double * prices, const double * quantities, const uint8_t * is_buy, size_t len, uint32_t tick_count, FloxBar * bars_out, uint32_t max_bars)`
- `uint32_t flox_aggregate_volume_bars(const int64_t * timestamps, const double * prices, const double * quantities, const uint8_t * is_buy, size_t len, double volume_threshold, FloxBar * bars_out, uint32_t max_bars)`

### composite_book

- `FloxCompositeBookHandle flox_composite_book_create(void)`
- `void flox_composite_book_destroy(FloxCompositeBookHandle book)`
- `uint8_t flox_composite_book_best_bid(FloxCompositeBookHandle book, uint32_t symbol, double * price_out, double * qty_out)`
- `uint8_t flox_composite_book_best_ask(FloxCompositeBookHandle book, uint32_t symbol, double * price_out, double * qty_out)`
- `uint8_t flox_composite_book_has_arb(FloxCompositeBookHandle book, uint32_t symbol)`
- `void flox_composite_book_mark_stale(FloxCompositeBookHandle book, uint32_t exchange, uint32_t symbol)`
- `void flox_composite_book_check_staleness(FloxCompositeBookHandle book, int64_t now_ns, int64_t threshold_ns)`

### context_queries

- `int64_t flox_position_raw(FloxStrategyHandle s, uint32_t symbol)`
- `int64_t flox_last_trade_price_raw(FloxStrategyHandle s, uint32_t symbol)`
- `int64_t flox_best_bid_raw(FloxStrategyHandle s, uint32_t symbol)`
- `int64_t flox_best_ask_raw(FloxStrategyHandle s, uint32_t symbol)`
- `int64_t flox_mid_price_raw(FloxStrategyHandle s, uint32_t symbol)`
- `void flox_get_symbol_context(FloxStrategyHandle s, uint32_t symbol, FloxSymbolContext * out)`
- `int32_t flox_get_order_status(FloxStrategyHandle s, uint64_t order_id)`

### data_reader

- `FloxDataReaderHandle flox_data_reader_create(const char * data_dir)`
- `void flox_data_reader_destroy(FloxDataReaderHandle reader)`
- `uint64_t flox_data_reader_count(FloxDataReaderHandle reader)`

### data_writer

- `FloxDataWriterHandle flox_data_writer_create(const char * output_dir, uint64_t max_segment_mb, uint8_t exchange_id)`
- `void flox_data_writer_destroy(FloxDataWriterHandle writer)`
- `uint8_t flox_data_writer_write_trade(FloxDataWriterHandle writer, int64_t exchange_ts_ns, int64_t recv_ts_ns, double price, double qty, uint64_t trade_id, uint32_t symbol_id, uint8_t side)`
- `void flox_data_writer_flush(FloxDataWriterHandle writer)`
- `void flox_data_writer_close(FloxDataWriterHandle writer)`

### datareader

- `FloxDataReaderHandle flox_data_reader_create_filtered(const char * data_dir, int64_t from_ns, int64_t to_ns, const uint32_t * symbols, uint32_t num_symbols)`
- `FloxDatasetSummary flox_data_reader_summary(FloxDataReaderHandle reader)`
- `FloxReaderStats flox_data_reader_stats(FloxDataReaderHandle reader)`
- `uint64_t flox_data_reader_read_trades(FloxDataReaderHandle reader, FloxTradeRecord * trades_out, uint64_t max_trades)`
- `uint64_t flox_data_reader_read_bbo(FloxDataReaderHandle reader, FloxBBO * bbos_out, uint64_t max_events)`
- `uint64_t flox_data_reader_count_book_updates(FloxDataReaderHandle reader, uint64_t * total_levels_out)`
- `uint64_t flox_data_reader_read_book_updates(FloxDataReaderHandle reader, FloxBookUpdateHeader * headers_out, uint64_t max_events, FloxLevel * levels_out, uint64_t max_levels)`
- `uint64_t flox_data_reader_read_trades_from(FloxDataReaderHandle reader, int64_t start_ts_ns, FloxTradeRecord * trades_out, uint64_t max_trades)`
- `uint64_t flox_data_reader_read_bbo_from(FloxDataReaderHandle reader, int64_t start_ts_ns, FloxBBO * bbos_out, uint64_t max_events)`
- `uint64_t flox_data_reader_count_book_updates_from(FloxDataReaderHandle reader, int64_t start_ts_ns, uint64_t * total_levels_out)`
- `uint64_t flox_data_reader_read_book_updates_from(FloxDataReaderHandle reader, int64_t start_ts_ns, FloxBookUpdateHeader * headers_out, uint64_t max_events, FloxLevel * levels_out, uint64_t max_levels)`

### datarecorder

- `FloxDataRecorderHandle flox_data_recorder_create(const char * output_dir, const char * exchange_name, uint64_t max_segment_mb)`
- `void flox_data_recorder_destroy(FloxDataRecorderHandle recorder)`
- `void flox_data_recorder_add_symbol(FloxDataRecorderHandle recorder, uint32_t symbol_id, const char * name, const char * base, const char * quote, int8_t price_precision, int8_t qty_precision)`
- `void flox_data_recorder_start(FloxDataRecorderHandle recorder)`
- `void flox_data_recorder_stop(FloxDataRecorderHandle recorder)`
- `void flox_data_recorder_flush(FloxDataRecorderHandle recorder)`
- `uint8_t flox_data_recorder_is_recording(FloxDataRecorderHandle recorder)`

### datawriter

- `FloxWriterStats flox_data_writer_stats(FloxDataWriterHandle writer)`

### delta_book

- `FloxDeltaBookEncoderHandle flox_delta_book_encoder_create(uint32_t anchor_every)`
- `void flox_delta_book_encoder_destroy(FloxDeltaBookEncoderHandle handle)`
- `void flox_delta_book_encoder_reset(FloxDeltaBookEncoderHandle handle, uint32_t symbol_id)`
- `void flox_delta_book_encoder_reset_all(FloxDeltaBookEncoderHandle handle)`
- `void flox_delta_book_encoder_encode(FloxDeltaBookEncoderHandle handle, uint32_t symbol_id, const FloxBookLevel * bids, size_t bid_count, const FloxBookLevel * asks, size_t ask_count, uint8_t * out_is_delta, uint64_t * out_bid_count, uint64_t * out_ask_count)`
- `uint64_t flox_delta_book_encoder_copy_bids(FloxDeltaBookEncoderHandle handle, FloxBookLevel * out, uint64_t max_entries)`
- `uint64_t flox_delta_book_encoder_copy_asks(FloxDeltaBookEncoderHandle handle, FloxBookLevel * out, uint64_t max_entries)`
- `FloxDeltaBookReplayerHandle flox_delta_book_replayer_create(void)`
- `void flox_delta_book_replayer_destroy(FloxDeltaBookReplayerHandle handle)`
- `void flox_delta_book_replayer_reset(FloxDeltaBookReplayerHandle handle, uint32_t symbol_id)`
- `void flox_delta_book_replayer_apply(FloxDeltaBookReplayerHandle handle, uint8_t type, uint32_t symbol_id, const FloxBookLevel * bids, size_t bid_count, const FloxBookLevel * asks, size_t ask_count, uint64_t * out_bid_count, uint64_t * out_ask_count)`
- `uint64_t flox_delta_book_replayer_copy_bids(FloxDeltaBookReplayerHandle handle, FloxBookLevel * out, uint64_t max_entries)`
- `uint64_t flox_delta_book_replayer_copy_asks(FloxDeltaBookReplayerHandle handle, FloxBookLevel * out, uint64_t max_entries)`

### execution

- `FloxExecutionListenerHandle flox_execution_listener_create(FloxExecutionListenerCallbacks callbacks)`
- `void flox_execution_listener_destroy(FloxExecutionListenerHandle listener)`
- `FloxExecutorHandle flox_executor_create(FloxExecutorCallbacks callbacks)`
- `void flox_executor_destroy(FloxExecutorHandle executor)`
- `void flox_executor_get_capabilities(FloxExecutorHandle executor, FloxExchangeCapabilities * caps_out)`
- `void flox_live_engine_set_executor(FloxLiveEngineHandle engine, FloxExecutorHandle executor)`
- `void flox_runner_set_executor(FloxRunnerHandle runner, FloxExecutorHandle executor)`
- `void flox_backtest_runner_add_execution_listener(FloxBacktestRunnerHandle runner, FloxExecutionListenerHandle listener)`
- `void flox_backtest_runner_set_executor(FloxBacktestRunnerHandle runner, FloxExecutorHandle executor)`

### execution_algos

- `FloxExecAlgoHandle flox_exec_twap_create(double target_qty, uint8_t side, uint32_t symbol, uint8_t type, double limit_price, int64_t duration_ns, uint32_t slice_count, int64_t start_time_ns)`
- `FloxExecAlgoHandle flox_exec_vwap_create(double target_qty, uint8_t side, uint32_t symbol, uint8_t type, double limit_price, const int64_t * volume_curve_ts, const double * volume_curve_vol, size_t n)`
- `FloxExecAlgoHandle flox_exec_iceberg_create(double target_qty, uint8_t side, uint32_t symbol, uint8_t type, double limit_price, double visible_qty)`
- `FloxExecAlgoHandle flox_exec_pov_create(double target_qty, uint8_t side, uint32_t symbol, uint8_t type, double limit_price, double participation_rate, double min_slice_qty)`
- `void flox_exec_destroy(FloxExecAlgoHandle handle)`
- `void flox_exec_step(FloxExecAlgoHandle handle, int64_t now_ns)`
- `void flox_exec_report_fill(FloxExecAlgoHandle handle, double qty)`
- `void flox_exec_observe_volume(FloxExecAlgoHandle handle, double qty)`
- `size_t flox_exec_pending_count(FloxExecAlgoHandle handle)`
- `uint8_t flox_exec_pending_at(FloxExecAlgoHandle handle, size_t index, FloxExecChildOrder * out)`
- `void flox_exec_clear_pending(FloxExecAlgoHandle handle)`
- `double flox_exec_target_qty(FloxExecAlgoHandle handle)`
- `double flox_exec_submitted_qty(FloxExecAlgoHandle handle)`
- `double flox_exec_filled_qty(FloxExecAlgoHandle handle)`
- `double flox_exec_remaining_qty(FloxExecAlgoHandle handle)`
- `uint8_t flox_exec_is_done(FloxExecAlgoHandle handle)`

### executor_fill

- `uint32_t flox_simulated_executor_get_fills(FloxSimulatedExecutorHandle executor, FloxFill * fills_out, uint32_t max_fills)`

### feed_clock

- `FloxFeedClockHandle flox_feed_clock_create(const uint32_t * symbols, uint32_t symbol_count, uint8_t policy, int64_t timeout_ms, uint32_t leader_symbol, int64_t staleness_budget_ms)`
- `void flox_feed_clock_destroy(FloxFeedClockHandle h)`
- `uint32_t flox_feed_clock_symbol_count(FloxFeedClockHandle h)`
- `uint32_t flox_feed_clock_symbol_at(FloxFeedClockHandle h, uint32_t index)`
- `uint8_t flox_feed_clock_tick(FloxFeedClockHandle h, int64_t ts_ns, uint32_t symbol)`
- `uint8_t flox_feed_clock_last_fired(FloxFeedClockHandle h)`
- `uint32_t flox_feed_clock_last_triggered_by(FloxFeedClockHandle h)`
- `int64_t flox_feed_clock_last_seen_at(FloxFeedClockHandle h, uint32_t index)`
- `int64_t flox_feed_clock_staleness_at(FloxFeedClockHandle h, uint32_t index)`
- `void flox_feed_clock_reset(FloxFeedClockHandle h)`

### fixed_point

- `int64_t flox_price_from_double(double value)`
- `double flox_price_to_double(int64_t raw)`
- `int64_t flox_quantity_from_double(double value)`
- `double flox_quantity_to_double(int64_t raw)`

### floxliveengine_disruptor

- `FloxLiveEngineHandle flox_live_engine_create(FloxRegistryHandle registry)`
- `void flox_live_engine_destroy(FloxLiveEngineHandle engine)`
- `void flox_live_engine_add_strategy(FloxLiveEngineHandle engine, FloxStrategyHandle strategy, FloxOnSignalCallback on_signal, void * user_data)`
- `void flox_live_engine_start(FloxLiveEngineHandle engine)`
- `void flox_live_engine_stop(FloxLiveEngineHandle engine)`
- `void flox_live_engine_publish_trade(FloxLiveEngineHandle engine, uint32_t symbol, double price, double qty, uint8_t is_buy, int64_t exchange_ts_ns)`
- `void flox_live_engine_publish_book_snapshot(FloxLiveEngineHandle engine, uint32_t symbol, const double * bid_prices, const double * bid_qtys, uint32_t n_bids, const double * ask_prices, const double * ask_qtys, uint32_t n_asks, int64_t exchange_ts_ns)`
- `void flox_live_engine_publish_bar(FloxLiveEngineHandle engine, uint32_t symbol, uint8_t bar_type, uint64_t bar_type_param, double open, double high, double low, double close, double volume, double buy_volume, int64_t start_time_ns, int64_t end_time_ns, uint8_t close_reason)`

### floxrun

- `FloxRunRecorderHandle flox_run_recorder_create(const char * path, const char * strategy_id, const char * strategy_hash, int64_t run_started_ns)`
- `void flox_run_recorder_destroy(FloxRunRecorderHandle handle)`
- `void flox_run_recorder_add_tape_ref(FloxRunRecorderHandle handle, const char * path, const char * content_hash, int64_t first_event_ns, int64_t last_event_ns)`
- `void flox_run_recorder_set_run_ended_ns(FloxRunRecorderHandle handle, int64_t ns)`
- `void flox_run_recorder_write_signal(FloxRunRecorderHandle handle, int64_t run_ts_ns, int64_t feed_ts_ns, uint32_t signal_id, uint32_t flags, int64_t strength_raw, const char * name, size_t name_len, const uint32_t * symbol_ids, size_t symbol_count, const uint8_t * payload, size_t payload_len)`
- `void flox_run_recorder_write_order_event(FloxRunRecorderHandle handle, int64_t run_ts_ns, int64_t feed_ts_ns, uint64_t order_id, uint64_t parent_signal_id, int64_t price_raw, int64_t qty_raw, uint32_t symbol_id, uint8_t event_kind, uint8_t side, uint8_t order_type, uint32_t flags, const char * reason, size_t reason_len)`
- `void flox_run_recorder_write_fill(FloxRunRecorderHandle handle, int64_t run_ts_ns, int64_t feed_ts_ns, uint64_t order_id, uint64_t fill_id, int64_t price_raw, int64_t qty_raw, int64_t fee_raw, uint32_t symbol_id, uint8_t side, uint8_t liquidity)`
- `void flox_run_recorder_close(FloxRunRecorderHandle handle)`
- `FloxRunReaderHandle flox_run_reader_open(const char * path)`
- `void flox_run_reader_close(FloxRunReaderHandle handle)`
- `uint64_t flox_run_reader_strategy_id(FloxRunReaderHandle handle, char * out, uint64_t max_bytes)`
- `uint64_t flox_run_reader_strategy_hash(FloxRunReaderHandle handle, char * out, uint64_t max_bytes)`
- `int64_t flox_run_reader_run_started_ns(FloxRunReaderHandle handle)`
- `int64_t flox_run_reader_run_ended_ns(FloxRunReaderHandle handle)`
- `uint64_t flox_run_reader_tape_ref_count(FloxRunReaderHandle handle)`
- `uint64_t flox_run_reader_tape_ref_path(FloxRunReaderHandle handle, uint64_t index, char * out, uint64_t max_bytes)`
- `uint64_t flox_run_reader_signal_count(FloxRunReaderHandle handle)`
- `uint64_t flox_run_reader_order_event_count(FloxRunReaderHandle handle)`
- `uint64_t flox_run_reader_fill_count(FloxRunReaderHandle handle)`
- `void flox_run_reader_signal_header(FloxRunReaderHandle handle, uint64_t index, int64_t * out_run_ts, int64_t * out_feed_ts, uint32_t * out_signal_id, uint32_t * out_flags, int64_t * out_strength_raw, uint64_t * out_name_len, uint64_t * out_symbol_count, uint64_t * out_payload_len)`
- `uint64_t flox_run_reader_signal_name(FloxRunReaderHandle handle, uint64_t index, char * out, uint64_t max_bytes)`
- `uint64_t flox_run_reader_signal_symbol_ids(FloxRunReaderHandle handle, uint64_t index, uint32_t * out, uint64_t max_entries)`
- `uint64_t flox_run_reader_signal_payload(FloxRunReaderHandle handle, uint64_t index, uint8_t * out, uint64_t max_bytes)`
- `void flox_run_reader_order_event_header(FloxRunReaderHandle handle, uint64_t index, int64_t * out_run_ts, int64_t * out_feed_ts, uint64_t * out_order_id, uint64_t * out_parent_signal_id, int64_t * out_price_raw, int64_t * out_qty_raw, uint32_t * out_symbol_id, uint8_t * out_event_kind, uint8_t * out_side, uint8_t * out_order_type, uint32_t * out_flags, uint64_t * out_reason_len)`
- `uint64_t flox_run_reader_order_event_reason(FloxRunReaderHandle handle, uint64_t index, char * out, uint64_t max_bytes)`
- `void flox_run_reader_fill(FloxRunReaderHandle handle, uint64_t index, int64_t * out_run_ts, int64_t * out_feed_ts, uint64_t * out_order_id, uint64_t * out_fill_id, int64_t * out_price_raw, int64_t * out_qty_raw, int64_t * out_fee_raw, uint32_t * out_symbol_id, uint8_t * out_side, uint8_t * out_liquidity)`

### footprint_bar

- `FloxFootprintHandle flox_footprint_create(double tick_size)`
- `void flox_footprint_destroy(FloxFootprintHandle footprint)`
- `void flox_footprint_add_trade(FloxFootprintHandle footprint, double price, double quantity, uint8_t is_buy)`
- `double flox_footprint_total_delta(FloxFootprintHandle footprint)`
- `double flox_footprint_total_volume(FloxFootprintHandle footprint)`
- `uint32_t flox_footprint_num_levels(FloxFootprintHandle footprint)`
- `void flox_footprint_clear(FloxFootprintHandle footprint)`

### grid_search

- `FloxGridSearchHandle flox_grid_search_create(void)`
- `void flox_grid_search_destroy(FloxGridSearchHandle gs)`
- `void flox_grid_search_add_axis(FloxGridSearchHandle gs, const double * values, uint32_t num_values)`
- `uint64_t flox_grid_search_total(FloxGridSearchHandle gs)`
- `uint32_t flox_grid_search_params_for_index(FloxGridSearchHandle gs, uint64_t index, double * params_out, uint32_t max_params)`
- `uint64_t flox_grid_search_run(FloxGridSearchHandle gs, FloxGridSearchFactoryFn factory, void * user_data, FloxBacktestStats * stats_out, uint32_t max_results)`

### heatmap

- `uint64_t flox_render_heatmap_html(const FloxHeatmapData * data, char * out_buf, uint64_t max_size)`

### heikin_ashi

- `uint32_t flox_aggregate_heikin_ashi_bars(const int64_t * timestamps, const double * prices, const double * quantities, const uint8_t * is_buy, size_t len, double interval_seconds, FloxBar * bars_out, uint32_t max_bars)`

### indicator_functions

- `void flox_indicator_ema(const double * input, size_t len, size_t period, double * output)`
- `void flox_indicator_sma(const double * input, size_t len, size_t period, double * output)`
- `void flox_indicator_rsi(const double * input, size_t len, size_t period, double * output)`
- `void flox_indicator_atr(const double * high, const double * low, const double * close, size_t len, size_t period, double * output)`
- `void flox_indicator_macd(const double * input, size_t len, size_t fast_period, size_t slow_period, size_t signal_period, double * macd_out, double * signal_out, double * hist_out)`
- `void flox_indicator_bollinger(const double * input, size_t len, size_t period, double multiplier, double * upper, double * middle, double * lower)`
- `void flox_indicator_rma(const double * input, size_t len, size_t period, double * output)`
- `void flox_indicator_dema(const double * input, size_t len, size_t period, double * output)`
- `void flox_indicator_tema(const double * input, size_t len, size_t period, double * output)`
- `void flox_indicator_kama(const double * input, size_t len, size_t period, size_t fast, size_t slow, double * output)`
- `void flox_indicator_slope(const double * input, size_t len, size_t length, double * output)`
- `void flox_indicator_adx(const double * high, const double * low, const double * close, size_t len, size_t period, double * adx_out, double * plus_di_out, double * minus_di_out)`
- `void flox_indicator_cci(const double * high, const double * low, const double * close, size_t len, size_t period, double * output)`
- `void flox_indicator_stochastic(const double * high, const double * low, const double * close, size_t len, size_t k_period, size_t d_period, double * k_out, double * d_out)`
- `void flox_indicator_chop(const double * high, const double * low, const double * close, size_t len, size_t period, double * output)`
- `void flox_indicator_obv(const double * close, const double * volume, size_t len, double * output)`
- `void flox_indicator_vwap(const double * close, const double * volume, size_t len, size_t window, double * output)`
- `void flox_indicator_cvd(const double * open, const double * high, const double * low, const double * close, const double * volume, size_t len, double * output)`
- `void flox_indicator_skewness(const double * input, size_t len, size_t period, double * output)`
- `void flox_indicator_kurtosis(const double * input, size_t len, size_t period, double * output)`
- `void flox_indicator_parkinson_vol(const double * high, const double * low, size_t len, size_t period, double * output)`
- `void flox_indicator_rogers_satchell_vol(const double * open, const double * high, const double * low, const double * close, size_t len, size_t period, double * output)`
- `void flox_indicator_rolling_zscore(const double * input, size_t len, size_t period, double * output)`
- `void flox_indicator_shannon_entropy(const double * input, size_t len, size_t period, size_t bins, double * output)`
- `void flox_indicator_correlation(const double * x, const double * y, size_t len, size_t period, double * output)`
- `void flox_indicator_adf(const double * input, size_t len, size_t max_lag, const char * regression, double * test_stat_out, double * p_value_out, size_t * used_lag_out)`
- `void flox_indicator_autocorrelation(const double * input, size_t len, size_t window, size_t lag, double * output)`

### indicatorgraph

- `FloxIndicatorGraphHandle flox_indicator_graph_create(void)`
- `void flox_indicator_graph_destroy(FloxIndicatorGraphHandle g)`
- `void flox_indicator_graph_set_bars(FloxIndicatorGraphHandle g, uint32_t symbol, const double * close, const double * high, const double * low, const double * volume, size_t len)`
- `void flox_indicator_graph_add_node(FloxIndicatorGraphHandle g, const char * name, const char *const * deps, size_t num_deps, FloxGraphNodeFn fn, void * user_data)`
- `const double * flox_indicator_graph_require(FloxIndicatorGraphHandle g, uint32_t symbol, const char * name, size_t * len_out)`
- `const double * flox_indicator_graph_get(FloxIndicatorGraphHandle g, uint32_t symbol, const char * name, size_t * len_out)`
- `const double * flox_indicator_graph_close(FloxIndicatorGraphHandle g, uint32_t symbol, size_t * len_out)`
- `const double * flox_indicator_graph_high(FloxIndicatorGraphHandle g, uint32_t symbol, size_t * len_out)`
- `const double * flox_indicator_graph_low(FloxIndicatorGraphHandle g, uint32_t symbol, size_t * len_out)`
- `const double * flox_indicator_graph_volume(FloxIndicatorGraphHandle g, uint32_t symbol, size_t * len_out)`
- `void flox_indicator_graph_invalidate(FloxIndicatorGraphHandle g, uint32_t symbol)`
- `void flox_indicator_graph_invalidate_all(FloxIndicatorGraphHandle g)`
- `void flox_indicator_graph_step(FloxIndicatorGraphHandle g, uint32_t symbol, double open, double high, double low, double close, double volume)`
- `double flox_indicator_graph_current(FloxIndicatorGraphHandle g, uint32_t symbol, const char * name)`
- `uint32_t flox_indicator_graph_bar_count(FloxIndicatorGraphHandle g, uint32_t symbol)`
- `void flox_indicator_graph_reset(FloxIndicatorGraphHandle g, uint32_t symbol)`
- `void flox_indicator_graph_reset_all(FloxIndicatorGraphHandle g)`
- `FloxStreamingGraphHandle flox_streaming_graph_create(void)`
- `void flox_streaming_graph_destroy(FloxStreamingGraphHandle sg)`
- `void flox_streaming_graph_add_node(FloxStreamingGraphHandle sg, const char * name, const char *const * deps, size_t num_deps, FloxGraphNodeFn fn, void * user_data)`
- `void flox_streaming_graph_step(FloxStreamingGraphHandle sg, uint32_t symbol, double open, double high, double low, double close, double volume)`
- `double flox_streaming_graph_current(FloxStreamingGraphHandle sg, uint32_t symbol, const char * name)`
- `uint32_t flox_streaming_graph_bar_count(FloxStreamingGraphHandle sg, uint32_t symbol)`
- `void flox_streaming_graph_reset(FloxStreamingGraphHandle sg, uint32_t symbol)`
- `void flox_streaming_graph_reset_all(FloxStreamingGraphHandle sg)`
- `const double * flox_streaming_graph_close(FloxStreamingGraphHandle sg, uint32_t symbol, size_t * len_out)`
- `const double * flox_streaming_graph_high(FloxStreamingGraphHandle sg, uint32_t symbol, size_t * len_out)`
- `const double * flox_streaming_graph_low(FloxStreamingGraphHandle sg, uint32_t symbol, size_t * len_out)`
- `const double * flox_streaming_graph_volume(FloxStreamingGraphHandle sg, uint32_t symbol, size_t * len_out)`

### l3_order

- `FloxL3BookHandle flox_l3_book_create(void)`
- `void flox_l3_book_destroy(FloxL3BookHandle book)`
- `int32_t flox_l3_book_add_order(FloxL3BookHandle book, uint64_t order_id, double price, double quantity, uint8_t side)`
- `int32_t flox_l3_book_remove_order(FloxL3BookHandle book, uint64_t order_id)`
- `int32_t flox_l3_book_modify_order(FloxL3BookHandle book, uint64_t order_id, double new_qty)`
- `uint8_t flox_l3_book_best_bid(FloxL3BookHandle book, double * price_out)`
- `uint8_t flox_l3_book_best_ask(FloxL3BookHandle book, double * price_out)`
- `double flox_l3_book_bid_at_price(FloxL3BookHandle book, double price)`
- `double flox_l3_book_ask_at_price(FloxL3BookHandle book, double price)`

### latency_models

- `FloxLatencyModelHandle flox_latency_constant_create(int64_t feed_ns, int64_t order_ns, int64_t fill_ns)`
- `FloxLatencyModelHandle flox_latency_gaussian_create(double feed_mean_ns, double feed_stddev_ns, double order_mean_ns, double order_stddev_ns, double fill_mean_ns, double fill_stddev_ns, uint64_t seed)`
- `FloxLatencyModelHandle flox_latency_exponential_create(double feed_mean_ns, double order_mean_ns, double fill_mean_ns, uint64_t seed)`
- `FloxLatencyModelHandle flox_latency_empirical_create(const int64_t * feed_samples, size_t feed_count, const int64_t * order_samples, size_t order_count, const int64_t * fill_samples, size_t fill_count, uint64_t seed)`
- `void flox_latency_destroy(FloxLatencyModelHandle model)`
- `int64_t flox_latency_feed_delay(FloxLatencyModelHandle model)`
- `int64_t flox_latency_order_delay(FloxLatencyModelHandle model)`
- `int64_t flox_latency_fill_delay(FloxLatencyModelHandle model)`
- `void flox_latency_sample(FloxLatencyModelHandle model, FloxLatencySample * out)`
- `void flox_latency_reset(FloxLatencyModelHandle model, uint64_t seed)`

### logger

- `void flox_set_log_callback(FloxLogCallback callback, void * user_data)`

### market_profile

- `FloxMarketProfileHandle flox_market_profile_create(double tick_size, uint32_t period_minutes, int64_t session_start_ns)`
- `void flox_market_profile_destroy(FloxMarketProfileHandle profile)`
- `void flox_market_profile_add_trade(FloxMarketProfileHandle profile, int64_t timestamp_ns, double price, double qty, uint8_t is_buy)`
- `double flox_market_profile_poc(FloxMarketProfileHandle profile)`
- `double flox_market_profile_vah(FloxMarketProfileHandle profile)`
- `double flox_market_profile_val(FloxMarketProfileHandle profile)`
- `double flox_market_profile_ib_high(FloxMarketProfileHandle profile)`
- `double flox_market_profile_ib_low(FloxMarketProfileHandle profile)`
- `uint8_t flox_market_profile_is_poor_high(FloxMarketProfileHandle profile)`
- `uint8_t flox_market_profile_is_poor_low(FloxMarketProfileHandle profile)`
- `uint32_t flox_market_profile_num_levels(FloxMarketProfileHandle profile)`
- `void flox_market_profile_clear(FloxMarketProfileHandle profile)`

### metrics

- `FloxPnLTrackerHandle flox_pnl_tracker_create(FloxPnLTrackerCallbacks callbacks)`
- `void flox_pnl_tracker_destroy(FloxPnLTrackerHandle tracker)`
- `void flox_live_engine_set_pnl_tracker(FloxLiveEngineHandle engine, FloxPnLTrackerHandle tracker)`
- `void flox_runner_set_pnl_tracker(FloxRunnerHandle runner, FloxPnLTrackerHandle tracker)`

### multi_tf_helpers

- `uint8_t flox_strategy_last_closed_bar(FloxStrategyHandle s, uint32_t symbol, uint8_t bar_type, uint64_t param, FloxBar * out)`
- `uint32_t flox_strategy_last_n_closed_bars(FloxStrategyHandle s, uint32_t symbol, uint8_t bar_type, uint64_t param, FloxBar * bars_out, uint32_t max_bars)`
- `uint32_t flox_strategy_get_bar_ring_capacity(FloxStrategyHandle s)`
- `void flox_strategy_set_bar_ring_capacity(FloxStrategyHandle s, uint32_t capacity)`

### order_book

- `FloxBookHandle flox_book_create(double tick_size)`
- `void flox_book_destroy(FloxBookHandle book)`
- `void flox_book_apply_snapshot(FloxBookHandle book, const double * bid_prices, const double * bid_qtys, size_t bid_len, const double * ask_prices, const double * ask_qtys, size_t ask_len)`
- `void flox_book_apply_delta(FloxBookHandle book, const double * bid_prices, const double * bid_qtys, size_t bid_len, const double * ask_prices, const double * ask_qtys, size_t ask_len)`
- `uint8_t flox_book_best_bid(FloxBookHandle book, double * price_out)`
- `uint8_t flox_book_best_ask(FloxBookHandle book, double * price_out)`
- `uint8_t flox_book_mid(FloxBookHandle book, double * price_out)`
- `uint8_t flox_book_spread(FloxBookHandle book, double * spread_out)`
- `double flox_book_bid_at_price(FloxBookHandle book, double price)`
- `double flox_book_ask_at_price(FloxBookHandle book, double price)`
- `uint8_t flox_book_is_crossed(FloxBookHandle book)`
- `void flox_book_clear(FloxBookHandle book)`
- `uint32_t flox_book_get_bids(FloxBookHandle book, double * prices_out, double * qtys_out, uint32_t max_levels)`
- `uint32_t flox_book_get_asks(FloxBookHandle book, double * prices_out, double * qtys_out, uint32_t max_levels)`

### order_group

- `FloxOrderGroupHandle flox_order_group_create(uint64_t parent_signal_id, uint8_t policy)`
- `void flox_order_group_destroy(FloxOrderGroupHandle h)`
- `uint32_t flox_order_group_add_market_leg(FloxOrderGroupHandle h, uint32_t symbol, uint8_t side, int64_t qty_raw)`
- `uint32_t flox_order_group_add_limit_leg(FloxOrderGroupHandle h, uint32_t symbol, uint8_t side, int64_t price_raw, int64_t qty_raw)`
- `uint32_t flox_order_group_leg_count(FloxOrderGroupHandle h)`
- `uint8_t flox_order_group_leg_state(FloxOrderGroupHandle h, uint32_t leg_index)`
- `int64_t flox_order_group_leg_filled_raw(FloxOrderGroupHandle h, uint32_t leg_index)`
- `uint64_t flox_order_group_leg_order_id(FloxOrderGroupHandle h, uint32_t leg_index)`
- `void flox_order_group_record_submit(FloxOrderGroupHandle h, uint32_t leg_index, uint64_t order_id)`
- `void flox_order_group_record_fill(FloxOrderGroupHandle h, uint32_t leg_index, int64_t cumulative_qty_raw)`
- `void flox_order_group_record_cancel(FloxOrderGroupHandle h, uint32_t leg_index)`
- `void flox_order_group_record_failure(FloxOrderGroupHandle h, uint32_t leg_index)`
- `uint8_t flox_order_group_state(FloxOrderGroupHandle h)`
- `uint32_t flox_order_group_recommended_actions(FloxOrderGroupHandle h, int64_t * actions_out, uint32_t max_actions)`
- `void flox_order_group_mark_action_dispatched(FloxOrderGroupHandle h, uint32_t leg_index, uint8_t kind)`

### order_tracker

- `FloxOrderTrackerHandle flox_order_tracker_create(void)`
- `void flox_order_tracker_destroy(FloxOrderTrackerHandle tracker)`
- `uint8_t flox_order_tracker_on_submitted(FloxOrderTrackerHandle tracker, uint64_t order_id, uint32_t symbol, uint8_t side, double price, double qty)`
- `uint8_t flox_order_tracker_on_filled(FloxOrderTrackerHandle tracker, uint64_t order_id, double fill_qty)`
- `uint8_t flox_order_tracker_on_canceled(FloxOrderTrackerHandle tracker, uint64_t order_id)`
- `uint8_t flox_order_tracker_is_active(FloxOrderTrackerHandle tracker, uint64_t order_id)`
- `uint32_t flox_order_tracker_active_count(FloxOrderTrackerHandle tracker)`
- `uint32_t flox_order_tracker_total_count(FloxOrderTrackerHandle tracker)`
- `void flox_order_tracker_prune(FloxOrderTrackerHandle tracker)`

### partitioner

- `FloxPartitionerHandle flox_partitioner_create(const char * data_dir)`
- `void flox_partitioner_destroy(FloxPartitionerHandle partitioner)`
- `uint32_t flox_partitioner_by_time(FloxPartitionerHandle p, uint32_t num_partitions, int64_t warmup_ns, FloxPartition * partitions_out, uint32_t max_partitions)`
- `uint32_t flox_partitioner_by_duration(FloxPartitionerHandle p, int64_t duration_ns, int64_t warmup_ns, FloxPartition * partitions_out, uint32_t max_partitions)`
- `uint32_t flox_partitioner_by_calendar(FloxPartitionerHandle p, uint8_t unit, int64_t warmup_ns, FloxPartition * partitions_out, uint32_t max_partitions)`
- `uint32_t flox_partitioner_by_symbol(FloxPartitionerHandle p, uint32_t num_partitions, FloxPartition * partitions_out, uint32_t max_partitions)`
- `uint32_t flox_partitioner_per_symbol(FloxPartitionerHandle p, FloxPartition * partitions_out, uint32_t max_partitions)`
- `uint32_t flox_partitioner_by_event_count(FloxPartitionerHandle p, uint32_t num_partitions, FloxPartition * partitions_out, uint32_t max_partitions)`

### pointer_out

- `void flox_data_reader_summary_p(FloxDataReaderHandle reader, void * out)`
- `void flox_data_reader_stats_p(FloxDataReaderHandle reader, void * out)`
- `void flox_data_writer_stats_p(FloxDataWriterHandle writer, void * out)`
- `void flox_segment_merge_full_p(const char * input_paths, size_t num_paths, const char * output_dir, const char * output_name, uint8_t sort, void * out)`
- `void flox_segment_merge_dir_p(const char * input_dir, const char * output_dir, void * out)`
- `void flox_segment_split_p(const char * input_path, const char * output_dir, uint8_t mode, int64_t time_interval_ns, uint64_t events_per_file, void * out)`
- `void flox_segment_export_p(const char * input_path, const char * output_path, uint8_t format, int64_t from_ns, int64_t to_ns, const uint32_t * symbols, uint32_t num_symbols, void * out)`
- `void flox_segment_validate_full_p(const char * path, uint8_t verify_crc, uint8_t verify_timestamps, void * out)`
- `void flox_dataset_validate_p(const char * data_dir, void * out)`

### portfolio_risk

- `FloxPortfolioRiskHandle flox_portfolio_risk_create(const FloxPortfolioRiskRules * rules, double initial_equity)`
- `void flox_portfolio_risk_destroy(FloxPortfolioRiskHandle handle)`
- `void flox_portfolio_risk_update(FloxPortfolioRiskHandle handle, const char * name, const FloxStrategyAccountFields * fields, uint8_t field_mask)`
- `void flox_portfolio_risk_remove(FloxPortfolioRiskHandle handle, const char * name)`
- `void flox_portfolio_risk_reset_kill_switch(FloxPortfolioRiskHandle handle)`
- `uint8_t flox_portfolio_risk_check_order(FloxPortfolioRiskHandle handle, const char * strategy, double notional, const char * side, FloxBreach * out_breach)`
- `double flox_portfolio_risk_total_daily_pnl(FloxPortfolioRiskHandle handle)`
- `double flox_portfolio_risk_total_gross_exposure(FloxPortfolioRiskHandle handle)`
- `double flox_portfolio_risk_current_equity(FloxPortfolioRiskHandle handle)`
- `double flox_portfolio_risk_drawdown_pct(FloxPortfolioRiskHandle handle)`
- `uint8_t flox_portfolio_risk_kill_switch_active(FloxPortfolioRiskHandle handle)`
- `uint64_t flox_portfolio_risk_breach_count(FloxPortfolioRiskHandle handle)`
- `uint8_t flox_portfolio_risk_breach_at(FloxPortfolioRiskHandle handle, uint64_t index, FloxBreach * out)`
- `uint64_t flox_portfolio_risk_account_count(FloxPortfolioRiskHandle handle)`

### position

- `FloxPositionTrackerHandle flox_position_tracker_create(uint8_t cost_basis)`
- `void flox_position_tracker_destroy(FloxPositionTrackerHandle tracker)`
- `void flox_position_tracker_on_fill(FloxPositionTrackerHandle tracker, uint32_t symbol, uint8_t side, double price, double quantity)`
- `double flox_position_tracker_position(FloxPositionTrackerHandle tracker, uint32_t symbol)`
- `double flox_position_tracker_avg_entry(FloxPositionTrackerHandle tracker, uint32_t symbol)`
- `double flox_position_tracker_realized_pnl(FloxPositionTrackerHandle tracker, uint32_t symbol)`
- `double flox_position_tracker_total_pnl(FloxPositionTrackerHandle tracker)`

### position_group

- `FloxPositionGroupHandle flox_position_group_create(void)`
- `void flox_position_group_destroy(FloxPositionGroupHandle tracker)`
- `uint64_t flox_position_group_open(FloxPositionGroupHandle tracker, uint64_t order_id, uint32_t symbol, uint8_t side, double price, double qty)`
- `void flox_position_group_close(FloxPositionGroupHandle tracker, uint64_t position_id, double exit_price)`
- `void flox_position_group_partial_close(FloxPositionGroupHandle tracker, uint64_t position_id, double qty, double exit_price)`
- `double flox_position_group_net_position(FloxPositionGroupHandle tracker, uint32_t symbol)`
- `double flox_position_group_realized_pnl(FloxPositionGroupHandle tracker, uint32_t symbol)`
- `double flox_position_group_total_pnl(FloxPositionGroupHandle tracker)`
- `uint32_t flox_position_group_open_count(FloxPositionGroupHandle tracker, uint32_t symbol)`
- `void flox_position_group_prune(FloxPositionGroupHandle tracker)`

### recorder

- `FloxMarketDataRecorderHandle flox_market_data_recorder_create(FloxMarketDataRecorderCallbacks callbacks)`
- `void flox_market_data_recorder_destroy(FloxMarketDataRecorderHandle recorder)`
- `void flox_live_engine_set_market_data_recorder(FloxLiveEngineHandle engine, FloxMarketDataRecorderHandle recorder)`
- `void flox_runner_set_market_data_recorder(FloxRunnerHandle runner, FloxMarketDataRecorderHandle recorder)`

### replay

- `FloxReplaySourceHandle flox_replay_source_create(FloxReplaySourceCallbacks callbacks)`
- `void flox_replay_source_destroy(FloxReplaySourceHandle source)`
- `uint8_t flox_replay_source_seek_to(FloxReplaySourceHandle source, int64_t timestamp_ns)`

### risk

- `FloxRiskManagerHandle flox_risk_manager_create(FloxRiskManagerCallbacks callbacks)`
- `void flox_risk_manager_destroy(FloxRiskManagerHandle rm)`
- `FloxKillSwitchHandle flox_kill_switch_create(FloxKillSwitchCallbacks callbacks)`
- `void flox_kill_switch_destroy(FloxKillSwitchHandle ks)`
- `FloxOrderValidatorHandle flox_order_validator_create(FloxOrderValidatorCallbacks callbacks)`
- `void flox_order_validator_destroy(FloxOrderValidatorHandle ov)`
- `void flox_live_engine_set_risk_manager(FloxLiveEngineHandle engine, FloxRiskManagerHandle rm)`
- `void flox_live_engine_set_kill_switch(FloxLiveEngineHandle engine, FloxKillSwitchHandle ks)`
- `void flox_live_engine_set_order_validator(FloxLiveEngineHandle engine, FloxOrderValidatorHandle ov)`
- `void flox_runner_set_risk_manager(FloxRunnerHandle runner, FloxRiskManagerHandle rm)`
- `void flox_runner_set_kill_switch(FloxRunnerHandle runner, FloxKillSwitchHandle ks)`
- `void flox_runner_set_order_validator(FloxRunnerHandle runner, FloxOrderValidatorHandle ov)`

### segment

- `uint8_t flox_segment_validate(const char * path)`
- `uint8_t flox_segment_merge(const char * input_dir, const char * output_path)`
- `FloxMergeResult flox_segment_merge_full(const char * input_paths, size_t num_paths, const char * output_dir, const char * output_name, uint8_t sort)`
- `FloxMergeResult flox_segment_merge_dir(const char * input_dir, const char * output_dir)`
- `FloxSplitResult flox_segment_split(const char * input_path, const char * output_dir, uint8_t mode, int64_t time_interval_ns, uint64_t events_per_file)`
- `FloxExportResult flox_segment_export(const char * input_path, const char * output_path, uint8_t format, int64_t from_ns, int64_t to_ns, const uint32_t * symbols, uint32_t num_symbols)`
- `uint8_t flox_segment_recompress(const char * input_path, const char * output_path, uint8_t compression)`
- `uint64_t flox_segment_extract_symbols(const char * input_path, const char * output_path, const uint32_t * symbols, uint32_t num_symbols)`
- `uint64_t flox_segment_extract_time_range(const char * input_path, const char * output_path, int64_t from_ns, int64_t to_ns)`

### signal_emission

- `uint64_t flox_emit_market_buy(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw)`
- `uint64_t flox_emit_market_sell(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw)`
- `uint64_t flox_emit_limit_buy(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw, int64_t qty_raw)`
- `uint64_t flox_emit_limit_sell(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw, int64_t qty_raw)`
- `void flox_emit_cancel(FloxStrategyHandle s, uint64_t order_id)`
- `void flox_emit_cancel_all(FloxStrategyHandle s, uint32_t symbol)`
- `void flox_emit_modify(FloxStrategyHandle s, uint64_t order_id, int64_t new_price_raw, int64_t new_qty_raw)`
- `uint64_t flox_emit_stop_market(FloxStrategyHandle s, uint32_t symbol, uint8_t side, int64_t trigger_raw, int64_t qty_raw)`
- `uint64_t flox_emit_stop_limit(FloxStrategyHandle s, uint32_t symbol, uint8_t side, int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw)`
- `uint64_t flox_emit_take_profit_market(FloxStrategyHandle s, uint32_t symbol, uint8_t side, int64_t trigger_raw, int64_t qty_raw)`
- `uint64_t flox_emit_trailing_stop(FloxStrategyHandle s, uint32_t symbol, uint8_t side, int64_t offset_raw, int64_t qty_raw)`
- `uint64_t flox_emit_trailing_stop_percent(FloxStrategyHandle s, uint32_t symbol, uint8_t side, int32_t callback_bps, int64_t qty_raw)`
- `uint64_t flox_emit_take_profit_limit(FloxStrategyHandle s, uint32_t symbol, uint8_t side, int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw)`
- `uint64_t flox_emit_limit_buy_tif(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw, int64_t qty_raw, uint8_t time_in_force)`
- `uint64_t flox_emit_limit_sell_tif(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw, int64_t qty_raw, uint8_t time_in_force)`
- `uint64_t flox_emit_close_position(FloxStrategyHandle s, uint32_t symbol)`

### simulated_executor

- `FloxSimulatedExecutorHandle flox_simulated_executor_create(void)`
- `void flox_simulated_executor_destroy(FloxSimulatedExecutorHandle executor)`
- `void flox_simulated_executor_submit_order(FloxSimulatedExecutorHandle executor, uint64_t id, uint8_t side, double price, double quantity, uint8_t order_type, uint32_t symbol)`
- `void flox_simulated_executor_cancel_order(FloxSimulatedExecutorHandle executor, uint64_t order_id)`
- `void flox_simulated_executor_cancel_all(FloxSimulatedExecutorHandle executor, uint32_t symbol)`
- `void flox_simulated_executor_on_bar(FloxSimulatedExecutorHandle executor, uint32_t symbol, double close_price)`
- `void flox_simulated_executor_on_trade(FloxSimulatedExecutorHandle executor, uint32_t symbol, double price, uint8_t is_buy)`
- `void flox_simulated_executor_advance_clock(FloxSimulatedExecutorHandle executor, int64_t timestamp_ns)`
- `uint32_t flox_simulated_executor_fill_count(FloxSimulatedExecutorHandle executor)`

### statistics

- `double flox_stat_correlation(const double * x, const double * y, size_t len)`
- `double flox_stat_profit_factor(const double * pnl, size_t len)`
- `double flox_stat_win_rate(const double * pnl, size_t len)`

### storage

- `FloxStorageSinkHandle flox_storage_sink_create(FloxStorageSinkCallbacks callbacks)`
- `void flox_storage_sink_destroy(FloxStorageSinkHandle sink)`
- `void flox_live_engine_set_storage_sink(FloxLiveEngineHandle engine, FloxStorageSinkHandle sink)`
- `void flox_runner_set_storage_sink(FloxRunnerHandle runner, FloxStorageSinkHandle sink)`

### strategy_lifecycle

- `FloxStrategyHandle flox_strategy_create(uint32_t id, const uint32_t * symbols, uint32_t num_symbols, FloxRegistryHandle registry, FloxStrategyCallbacks callbacks)`
- `void flox_strategy_destroy(FloxStrategyHandle strategy)`
- `void flox_strategy_replace_callbacks(FloxStrategyHandle strategy, FloxStrategyCallbacks callbacks)`

### strategyrunner_synchronous

- `FloxRunnerHandle flox_runner_create(FloxRegistryHandle registry, FloxOnSignalCallback on_signal, void * user_data)`
- `void flox_runner_destroy(FloxRunnerHandle runner)`
- `void flox_runner_add_strategy(FloxRunnerHandle runner, FloxStrategyHandle strategy)`
- `void flox_runner_start(FloxRunnerHandle runner)`
- `void flox_runner_stop(FloxRunnerHandle runner)`
- `void flox_runner_on_trade(FloxRunnerHandle runner, uint32_t symbol, double price, double qty, uint8_t is_buy, int64_t exchange_ts_ns)`
- `void flox_runner_on_book_snapshot(FloxRunnerHandle runner, uint32_t symbol, const double * bid_prices, const double * bid_qtys, uint32_t n_bids, const double * ask_prices, const double * ask_qtys, uint32_t n_asks, int64_t exchange_ts_ns)`
- `void flox_runner_on_bar(FloxRunnerHandle runner, uint32_t symbol, uint8_t bar_type, uint64_t bar_type_param, double open, double high, double low, double close, double volume, double buy_volume, int64_t start_time_ns, int64_t end_time_ns, uint8_t close_reason)`

### symbol_registry

- `FloxRegistryHandle flox_registry_create(void)`
- `void flox_registry_destroy(FloxRegistryHandle registry)`
- `uint32_t flox_registry_add_symbol(FloxRegistryHandle registry, const char * exchange, const char * name, double tick_size)`
- `uint8_t flox_registry_get_symbol_id(FloxRegistryHandle registry, const char * exchange, const char * name, uint32_t * id_out)`
- `uint8_t flox_registry_get_symbol_name(FloxRegistryHandle registry, uint32_t symbol_id, char * exchange_out, size_t exchange_len, char * name_out, size_t name_len)`
- `uint32_t flox_registry_symbol_count(FloxRegistryHandle registry)`

### tape_diff

- `FloxTapeDiffHandle flox_tape_diff_create(const char * left_path, const char * right_path, uint32_t max_mismatches, int64_t field_tolerance_ns)`
- `void flox_tape_diff_destroy(FloxTapeDiffHandle handle)`
- `uint64_t flox_tape_diff_left_count(FloxTapeDiffHandle handle)`
- `uint64_t flox_tape_diff_right_count(FloxTapeDiffHandle handle)`
- `uint8_t flox_tape_diff_first_divergence(FloxTapeDiffHandle handle, uint64_t * out_index)`
- `uint8_t flox_tape_diff_equal(FloxTapeDiffHandle handle)`
- `uint64_t flox_tape_diff_mismatch_count(FloxTapeDiffHandle handle)`
- `uint64_t flox_tape_diff_copy_mismatches(FloxTapeDiffHandle handle, FloxTapeDiffMismatch * out, uint64_t max_entries)`

### targets

- `void flox_target_future_return(const double * close, size_t len, size_t horizon, double * output)`
- `void flox_target_future_ctc_volatility(const double * close, size_t len, size_t horizon, double * output)`
- `void flox_target_future_linear_slope(const double * close, size_t len, size_t horizon, double * output)`

### trace_attach

- `void flox_runner_attach_trace_recorder(FloxRunnerHandle runner, FloxRunRecorderHandle recorder)`
- `void flox_runner_set_trace_feed_ts_ns(FloxRunnerHandle runner, int64_t feed_ts_ns)`
- `void flox_runner_trace_order_event(FloxRunnerHandle runner, uint64_t order_id, uint64_t parent_signal_id, uint32_t symbol_id, uint8_t event_kind, uint8_t side, uint8_t order_type, int64_t price_raw, int64_t qty_raw, uint32_t flags)`
- `void flox_runner_trace_fill(FloxRunnerHandle runner, uint64_t order_id, uint64_t fill_id, int64_t price_raw, int64_t qty_raw, int64_t fee_raw, uint32_t symbol_id, uint8_t side, uint8_t liquidity)`

### validation

- `FloxSegmentValidation flox_segment_validate_full(const char * path, uint8_t verify_crc, uint8_t verify_timestamps)`
- `FloxDatasetValidation flox_dataset_validate(const char * data_dir)`

### volume_profile

- `FloxVolumeProfileHandle flox_volume_profile_create(double tick_size)`
- `void flox_volume_profile_destroy(FloxVolumeProfileHandle profile)`
- `void flox_volume_profile_add_trade(FloxVolumeProfileHandle profile, double price, double quantity, uint8_t is_buy)`
- `double flox_volume_profile_poc(FloxVolumeProfileHandle profile)`
- `double flox_volume_profile_vah(FloxVolumeProfileHandle profile)`
- `double flox_volume_profile_val(FloxVolumeProfileHandle profile)`
- `double flox_volume_profile_total_volume(FloxVolumeProfileHandle profile)`
- `double flox_volume_profile_total_delta(FloxVolumeProfileHandle profile)`
- `uint32_t flox_volume_profile_num_levels(FloxVolumeProfileHandle profile)`
- `void flox_volume_profile_clear(FloxVolumeProfileHandle profile)`

### walk_forward

- `uint32_t flox_walk_forward_run_csv(FloxRegistryHandle registry, const char * csv_path, const char * symbol, double fee_rate, double initial_capital, const FloxWalkForwardConfig * cfg, FloxWalkForwardFactoryFn factory, void * user_data, FloxWalkForwardFold * folds_out, uint32_t max_folds)`

