# C API reference

```c
#include "flox/capi/flox_capi.h"
```

All language bindings (Python, Node.js, Codon, embedded JS) call into this header. You can use it directly to integrate Flox into any language with a C FFI, or embed it in a C/C++ project.

---

## Opaque handles

```c
typedef void* FloxRegistryHandle;
typedef void* FloxStrategyHandle;
typedef void* FloxRunnerHandle;
typedef void* FloxLiveEngineHandle;
typedef void* FloxBacktestRunnerHandle;
typedef void* FloxBacktestResultHandle;
typedef void* FloxExecutorHandle;
typedef void* FloxBookHandle;
typedef void* FloxL3BookHandle;
typedef void* FloxCompositeBookHandle;
typedef void* FloxPositionTrackerHandle;
typedef void* FloxPositionGroupHandle;
typedef void* FloxOrderTrackerHandle;
typedef void* FloxVolumeProfileHandle;
typedef void* FloxMarketProfileHandle;
typedef void* FloxFootprintHandle;
typedef void* FloxDataWriterHandle;
typedef void* FloxDataReaderHandle;
typedef void* FloxDataRecorderHandle;
typedef void* FloxPartitionerHandle;
```

---

## Structs

### `FloxTradeData`

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `uint32_t` | Symbol ID |
| `price_raw` | `int64_t` | Price × 1e8 |
| `quantity_raw` | `int64_t` | Quantity × 1e8 |
| `is_buy` | `uint8_t` | 1 = buy, 0 = sell |
| `exchange_ts_ns` | `int64_t` | Exchange timestamp (ns) |

### `FloxBookLevel`

| Field | Type | Description |
|-------|------|-------------|
| `price_raw` | `int64_t` | Price × 1e8 |
| `quantity_raw` | `int64_t` | Quantity × 1e8 |

### `FloxBookSnapshot`

| Field | Type | Description |
|-------|------|-------------|
| `bid_price_raw` | `int64_t` | Best bid price × 1e8 (0 if absent) |
| `bid_qty_raw` | `int64_t` | Best bid quantity × 1e8 |
| `ask_price_raw` | `int64_t` | Best ask price × 1e8 (0 if absent) |
| `ask_qty_raw` | `int64_t` | Best ask quantity × 1e8 |
| `mid_raw` | `int64_t` | Mid price × 1e8 (0 if absent) |
| `spread_raw` | `int64_t` | Spread × 1e8 (0 if absent) |

### `FloxBookData`

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `uint32_t` | Symbol ID |
| `exchange_ts_ns` | `int64_t` | Exchange timestamp (ns) |
| `snapshot` | `FloxBookSnapshot` | Top-of-book snapshot |

### `FloxSymbolContext`

| Field | Type | Description |
|-------|------|-------------|
| `symbol_id` | `uint32_t` | Symbol ID |
| `position_raw` | `int64_t` | Position × 1e8 |
| `avg_entry_price_raw` | `int64_t` | Average entry price × 1e8 |
| `last_trade_price_raw` | `int64_t` | Last trade price × 1e8 |
| `last_update_ns` | `int64_t` | Last update timestamp (ns) |
| `book` | `FloxBookSnapshot` | Top-of-book snapshot |

### `FloxStrategyCallbacks`

| Field | Type | Description |
|-------|------|-------------|
| `on_trade` | `FloxOnTradeCallback` | Trade event callback |
| `on_book` | `FloxOnBookCallback` | Book update callback |
| `on_start` | `FloxOnStartCallback` | Strategy start callback |
| `on_stop` | `FloxOnStopCallback` | Strategy stop callback |
| `user_data` | `void*` | Passed to all callbacks |

### `FloxSignal`

Emitted by strategies, received by the order backend.

| Field | Type | Description |
|-------|------|-------------|
| `order_id` | `uint64_t` | Order ID |
| `symbol` | `uint32_t` | Symbol ID |
| `side` | `uint8_t` | 0 = buy, 1 = sell |
| `order_type` | `uint8_t` | 0=market, 1=limit, 2=stop_market, 3=stop_limit, 4=tp_market, 5=tp_limit, 6=trailing_stop, 7=cancel, 8=cancel_all, 9=modify |
| `price` | `double` | Limit price (0 for market orders) |
| `quantity` | `double` | Order quantity |
| `trigger_price` | `double` | Stop/take-profit trigger |
| `trailing_offset` | `double` | Trailing stop absolute offset |
| `trailing_bps` | `int32_t` | Trailing stop callback rate (basis points) |
| `new_price` | `double` | Modify: updated price |
| `new_quantity` | `double` | Modify: updated quantity |

### `FloxBar`

| Field | Type | Description |
|-------|------|-------------|
| `start_time_ns` | `int64_t` | Bar open time (ns) |
| `end_time_ns` | `int64_t` | Bar close time (ns) |
| `open_raw` | `int64_t` | Open × 1e8 |
| `high_raw` | `int64_t` | High × 1e8 |
| `low_raw` | `int64_t` | Low × 1e8 |
| `close_raw` | `int64_t` | Close × 1e8 |
| `volume_raw` | `int64_t` | Volume × 1e8 |
| `buy_volume_raw` | `int64_t` | Buy-side volume × 1e8 |
| `trade_count` | `uint32_t` | Number of trades |

### `FloxFill`

| Field | Type | Description |
|-------|------|-------------|
| `order_id` | `uint64_t` | Order ID |
| `symbol` | `uint32_t` | Symbol ID |
| `side` | `uint8_t` | 0 = buy, 1 = sell |
| `price_raw` | `int64_t` | Fill price × 1e8 |
| `quantity_raw` | `int64_t` | Fill quantity × 1e8 |
| `timestamp_ns` | `int64_t` | Fill timestamp (ns) |

### `FloxBacktestStats`

| Field | Type | Description |
|-------|------|-------------|
| `totalTrades` | `uint64_t` | Round-trip trade count |
| `winningTrades` | `uint64_t` | Winning trades |
| `losingTrades` | `uint64_t` | Losing trades |
| `maxConsecutiveWins` | `uint64_t` | Max consecutive wins |
| `maxConsecutiveLosses` | `uint64_t` | Max consecutive losses |
| `initialCapital` | `double` | Starting capital |
| `finalCapital` | `double` | Ending capital |
| `totalPnl` | `double` | Gross PnL |
| `totalFees` | `double` | Total fees paid |
| `netPnl` | `double` | Net PnL after fees |
| `grossProfit` | `double` | Sum of winning trades |
| `grossLoss` | `double` | Sum of losing trades |
| `maxDrawdown` | `double` | Max drawdown (absolute) |
| `maxDrawdownPct` | `double` | Max drawdown (%) |
| `winRate` | `double` | Winning trade ratio |
| `profitFactor` | `double` | Gross profit / gross loss |
| `avgWin` | `double` | Average winning trade |
| `avgLoss` | `double` | Average losing trade |
| `avgWinLossRatio` | `double` | avgWin / avgLoss |
| `avgTradeDurationNs` | `double` | Average trade duration (ns) |
| `medianTradeDurationNs` | `double` | Median trade duration (ns) |
| `maxTradeDurationNs` | `double` | Longest trade (ns) |
| `sharpeRatio` | `double` | Annualized Sharpe ratio |
| `sortinoRatio` | `double` | Sortino ratio |
| `calmarRatio` | `double` | Calmar ratio |
| `timeWeightedReturn` | `double` | Time-weighted return |
| `returnPct` | `double` | Net return (%) |
| `startTimeNs` | `int64_t` | Backtest start timestamp (ns) |
| `endTimeNs` | `int64_t` | Backtest end timestamp (ns) |

### `FloxEquityPoint`

| Field | Type | Description |
|-------|------|-------------|
| `timestamp_ns` | `int64_t` | Timestamp (ns) |
| `equity` | `double` | Equity at this point |
| `drawdown_pct` | `double` | Drawdown (%) at this point |

---

## Callback types

```c
typedef void (*FloxOnTradeCallback)(void* user_data, const FloxSymbolContext* ctx,
                                    const FloxTradeData* trade);
typedef void (*FloxOnBookCallback)(void* user_data, const FloxSymbolContext* ctx,
                                   const FloxBookData* book);
typedef void (*FloxOnStartCallback)(void* user_data);
typedef void (*FloxOnStopCallback)(void* user_data);
typedef void (*FloxOnSignalCallback)(void* user_data, const FloxSignal* signal);
```

---

## Symbol registry

```c
FloxRegistryHandle flox_registry_create(void);
void               flox_registry_destroy(FloxRegistryHandle registry);

uint32_t flox_registry_add_symbol(FloxRegistryHandle registry,
                                  const char* exchange, const char* name,
                                  double tick_size);

uint8_t  flox_registry_get_symbol_id(FloxRegistryHandle registry,
                                     const char* exchange, const char* name,
                                     uint32_t* id_out);
uint8_t  flox_registry_get_symbol_name(FloxRegistryHandle registry,
                                       uint32_t symbol_id,
                                       char* exchange_out, size_t exchange_len,
                                       char* name_out, size_t name_len);
uint32_t flox_registry_symbol_count(FloxRegistryHandle registry);
```

---

## Strategy

```c
FloxStrategyHandle flox_strategy_create(uint32_t id,
                                        const uint32_t* symbols, uint32_t num_symbols,
                                        FloxRegistryHandle registry,
                                        FloxStrategyCallbacks callbacks);
void flox_strategy_destroy(FloxStrategyHandle strategy);
```

---

## StrategyRunner

Synchronous strategy host. Strategy callbacks fire in the caller's thread before the push call returns.

```c
FloxRunnerHandle flox_runner_create(FloxRegistryHandle registry,
                                    FloxOnSignalCallback on_signal,
                                    void* user_data);
void flox_runner_destroy(FloxRunnerHandle runner);

void flox_runner_add_strategy(FloxRunnerHandle runner, FloxStrategyHandle strategy);
void flox_runner_start(FloxRunnerHandle runner);
void flox_runner_stop(FloxRunnerHandle runner);

void flox_runner_on_trade(FloxRunnerHandle runner, uint32_t symbol,
                          double price, double qty, uint8_t is_buy,
                          int64_t exchange_ts_ns);
void flox_runner_on_book_snapshot(FloxRunnerHandle runner, uint32_t symbol,
                                  const double* bid_prices, const double* bid_qtys,
                                  uint32_t n_bids,
                                  const double* ask_prices, const double* ask_qtys,
                                  uint32_t n_asks, int64_t exchange_ts_ns);
```

---

## LiveEngine

Disruptor-based live trading engine. Each strategy runs in its own consumer thread. Publish calls are lock-free and return immediately.

```c
FloxLiveEngineHandle flox_live_engine_create(FloxRegistryHandle registry);
void                 flox_live_engine_destroy(FloxLiveEngineHandle engine);

void flox_live_engine_add_strategy(FloxLiveEngineHandle engine,
                                   FloxStrategyHandle strategy,
                                   FloxOnSignalCallback on_signal,
                                   void* user_data);

void flox_live_engine_start(FloxLiveEngineHandle engine);
void flox_live_engine_stop(FloxLiveEngineHandle engine);

void flox_live_engine_publish_trade(FloxLiveEngineHandle engine,
                                    uint32_t symbol,
                                    double price, double qty, uint8_t is_buy,
                                    int64_t exchange_ts_ns);
void flox_live_engine_publish_book_snapshot(FloxLiveEngineHandle engine,
                                            uint32_t symbol,
                                            const double* bid_prices,
                                            const double* bid_qtys, uint32_t n_bids,
                                            const double* ask_prices,
                                            const double* ask_qtys, uint32_t n_asks,
                                            int64_t exchange_ts_ns);
```

---

## BacktestRunner

Replays OHLCV data through a strategy and returns statistics.

```c
FloxBacktestRunnerHandle flox_backtest_runner_create(FloxRegistryHandle registry,
                                                     double fee_rate,
                                                     double initial_capital);
void flox_backtest_runner_destroy(FloxBacktestRunnerHandle runner);

void flox_backtest_runner_set_strategy(FloxBacktestRunnerHandle runner,
                                       FloxStrategyHandle strategy);

// Replay a CSV file (columns: timestamp, open, high, low, close, volume).
// Returns 1 on success, 0 on error.
int flox_backtest_runner_run_csv(FloxBacktestRunnerHandle runner,
                                 const char* path, const char* symbol,
                                 FloxBacktestStats* stats_out);

// Replay raw OHLCV arrays (timestamps in nanoseconds).
// Returns 1 on success, 0 on error.
int flox_backtest_runner_run_ohlcv(FloxBacktestRunnerHandle runner,
                                   const int64_t* timestamps_ns,
                                   const double* close_prices, uint32_t n,
                                   const char* symbol,
                                   FloxBacktestStats* stats_out);
```

---

## Signal emission

All return `OrderId` (`uint64_t`), 0 on failure. `cancel` and `modify` return void.

| Function | Description |
|----------|-------------|
| `flox_emit_market_buy(s, sym, qty_raw)` | Market buy |
| `flox_emit_market_sell(s, sym, qty_raw)` | Market sell |
| `flox_emit_limit_buy(s, sym, px_raw, qty_raw)` | Limit buy |
| `flox_emit_limit_sell(s, sym, px_raw, qty_raw)` | Limit sell |
| `flox_emit_limit_buy_tif(s, sym, px_raw, qty_raw, tif)` | Limit buy with time-in-force |
| `flox_emit_limit_sell_tif(s, sym, px_raw, qty_raw, tif)` | Limit sell with time-in-force |
| `flox_emit_stop_market(s, sym, side, trigger_raw, qty_raw)` | Stop market |
| `flox_emit_stop_limit(s, sym, side, trigger_raw, limit_raw, qty_raw)` | Stop limit |
| `flox_emit_take_profit_market(s, sym, side, trigger_raw, qty_raw)` | Take-profit market |
| `flox_emit_take_profit_limit(s, sym, side, trigger_raw, limit_raw, qty_raw)` | Take-profit limit |
| `flox_emit_trailing_stop(s, sym, side, offset_raw, qty_raw)` | Trailing stop (absolute) |
| `flox_emit_trailing_stop_percent(s, sym, side, bps, qty_raw)` | Trailing stop (basis points) |
| `flox_emit_close_position(s, sym)` | Close position (reduce-only) |
| `flox_emit_cancel(s, order_id)` | Cancel order |
| `flox_emit_cancel_all(s, sym)` | Cancel all orders for symbol |
| `flox_emit_modify(s, order_id, new_price_raw, new_qty_raw)` | Modify order |

---

## Context queries

| Function | Returns | Description |
|----------|---------|-------------|
| `flox_position_raw(s, sym)` | `int64_t` | Position × 1e8 |
| `flox_last_trade_price_raw(s, sym)` | `int64_t` | Last trade price × 1e8 |
| `flox_best_bid_raw(s, sym)` | `int64_t` | Best bid × 1e8 |
| `flox_best_ask_raw(s, sym)` | `int64_t` | Best ask × 1e8 |
| `flox_mid_price_raw(s, sym)` | `int64_t` | Mid price × 1e8 |
| `flox_get_symbol_context(s, sym, out)` | `void` | Fill `FloxSymbolContext` |
| `flox_get_order_status(s, order_id)` | `int32_t` | Order status (-1 = not found) |

---

## Simulated executor

Used in backtesting to fill orders from simulated market data.

```c
FloxExecutorHandle flox_executor_create(void);
void               flox_executor_destroy(FloxExecutorHandle executor);

void flox_executor_submit_order(FloxExecutorHandle executor,
                                uint64_t id, uint8_t side, double price,
                                double quantity, uint8_t order_type, uint32_t symbol);
void flox_executor_cancel_order(FloxExecutorHandle executor, uint64_t order_id);
void flox_executor_cancel_all(FloxExecutorHandle executor, uint32_t symbol);

// Feed market data
void flox_executor_on_bar(FloxExecutorHandle executor, uint32_t symbol, double close_price);
void flox_executor_on_trade(FloxExecutorHandle executor, uint32_t symbol,
                            double price, uint8_t is_buy);
void flox_executor_on_trade_qty(FloxExecutorHandle executor, uint32_t symbol,
                                double price, double quantity, uint8_t is_buy);
void flox_executor_on_best_levels(FloxExecutorHandle executor, uint32_t symbol,
                                  double bid_price, double bid_qty,
                                  double ask_price, double ask_qty);
void flox_executor_on_book_snapshot(FloxExecutorHandle executor, uint32_t symbol,
                                    const double* bid_prices, const double* bid_qtys,
                                    uint32_t n_bids,
                                    const double* ask_prices, const double* ask_qtys,
                                    uint32_t n_asks);
void flox_executor_advance_clock(FloxExecutorHandle executor, int64_t timestamp_ns);

// Fills
uint32_t flox_executor_fill_count(FloxExecutorHandle executor);
uint32_t flox_executor_get_fills(FloxExecutorHandle executor,
                                 FloxFill* fills_out, uint32_t max_fills);
```

### Slippage configuration

```c
typedef enum {
    FLOX_SLIPPAGE_NONE         = 0,
    FLOX_SLIPPAGE_FIXED_TICKS  = 1,
    FLOX_SLIPPAGE_FIXED_BPS    = 2,
    FLOX_SLIPPAGE_VOLUME_IMPACT = 3
} FloxSlippageModel;

void flox_executor_set_default_slippage(FloxExecutorHandle executor,
                                        int32_t model, int32_t ticks,
                                        double tick_size, double bps,
                                        double impact_coeff);
void flox_executor_set_symbol_slippage(FloxExecutorHandle executor, uint32_t symbol,
                                       int32_t model, int32_t ticks,
                                       double tick_size, double bps,
                                       double impact_coeff);
```

### Queue simulation

```c
typedef enum {
    FLOX_QUEUE_NONE = 0,
    FLOX_QUEUE_TOB  = 1,
    FLOX_QUEUE_FULL = 2
} FloxQueueModel;

void flox_executor_set_queue_model(FloxExecutorHandle executor,
                                   int32_t model, uint32_t depth);
```

---

## BacktestResult

Aggregates fills into trades, statistics, and equity curve.

```c
FloxBacktestResultHandle flox_backtest_result_create(double initial_capital,
                                                     double fee_rate,
                                                     uint8_t use_percentage_fee,
                                                     double fixed_fee_per_trade,
                                                     double risk_free_rate,
                                                     double annualization_factor);
void flox_backtest_result_destroy(FloxBacktestResultHandle result);

void flox_backtest_result_record_fill(FloxBacktestResultHandle result,
                                      uint64_t order_id, uint32_t symbol, uint8_t side,
                                      double price, double quantity, int64_t timestamp_ns);
void flox_backtest_result_ingest_executor(FloxBacktestResultHandle result,
                                          FloxExecutorHandle executor);

void     flox_backtest_result_stats(FloxBacktestResultHandle result, FloxBacktestStats* out);
uint32_t flox_backtest_result_equity_curve(FloxBacktestResultHandle result,
                                           FloxEquityPoint* points_out, uint32_t max_points);
uint8_t  flox_backtest_result_write_equity_curve_csv(FloxBacktestResultHandle result,
                                                     const char* path);
```

---

## Indicators

Stateless, array-in / array-out.

| Function | Description |
|----------|-------------|
| `flox_indicator_ema(input, len, period, output)` | EMA |
| `flox_indicator_sma(input, len, period, output)` | SMA |
| `flox_indicator_rsi(input, len, period, output)` | RSI |
| `flox_indicator_rma(input, len, period, output)` | Wilder's moving average |
| `flox_indicator_dema(input, len, period, output)` | Double EMA |
| `flox_indicator_tema(input, len, period, output)` | Triple EMA |
| `flox_indicator_kama(input, len, period, fast, slow, output)` | Kaufman adaptive MA |
| `flox_indicator_slope(input, len, length, output)` | Linear slope |
| `flox_indicator_atr(high, low, close, len, period, output)` | ATR |
| `flox_indicator_adx(high, low, close, len, period, adx, +di, -di)` | ADX |
| `flox_indicator_macd(input, len, fast, slow, signal, macd, signal, hist)` | MACD |
| `flox_indicator_bollinger(input, len, period, mult, upper, middle, lower)` | Bollinger Bands |
| `flox_indicator_cci(high, low, close, len, period, output)` | CCI |
| `flox_indicator_stochastic(high, low, close, len, k, d, k_out, d_out)` | Stochastic |
| `flox_indicator_chop(high, low, close, len, period, output)` | Choppiness |
| `flox_indicator_obv(close, volume, len, output)` | On-balance volume |
| `flox_indicator_vwap(close, volume, len, window, output)` | Rolling VWAP |
| `flox_indicator_cvd(open, high, low, close, volume, len, output)` | Cumulative volume delta |

---

## Bar aggregation

All functions return the number of bars written.

| Function | Description |
|----------|-------------|
| `flox_aggregate_time_bars(..., interval_seconds, bars_out, max)` | Time bars |
| `flox_aggregate_tick_bars(..., tick_count, bars_out, max)` | Tick bars |
| `flox_aggregate_volume_bars(..., volume_threshold, bars_out, max)` | Volume bars |
| `flox_aggregate_range_bars(..., range_size, bars_out, max)` | Range bars |
| `flox_aggregate_renko_bars(..., brick_size, bars_out, max)` | Renko bars |
| `flox_aggregate_heikin_ashi_bars(..., interval_seconds, bars_out, max)` | Heikin-Ashi |

All take the same input signature: `(timestamps, prices, quantities, is_buy, len, ...)`.

---

## L2 Order book

```c
FloxBookHandle flox_book_create(double tick_size);
void           flox_book_destroy(FloxBookHandle book);

void    flox_book_apply_snapshot(FloxBookHandle book,
                                 const double* bid_prices, const double* bid_qtys, size_t bid_len,
                                 const double* ask_prices, const double* ask_qtys, size_t ask_len);
void    flox_book_apply_delta(FloxBookHandle book,
                              const double* bid_prices, const double* bid_qtys, size_t bid_len,
                              const double* ask_prices, const double* ask_qtys, size_t ask_len);

uint8_t flox_book_best_bid(FloxBookHandle book, double* price_out);
uint8_t flox_book_best_ask(FloxBookHandle book, double* price_out);
uint8_t flox_book_mid(FloxBookHandle book, double* price_out);
uint8_t flox_book_spread(FloxBookHandle book, double* spread_out);
double  flox_book_bid_at_price(FloxBookHandle book, double price);
double  flox_book_ask_at_price(FloxBookHandle book, double price);
uint8_t flox_book_is_crossed(FloxBookHandle book);
void    flox_book_clear(FloxBookHandle book);

uint32_t flox_book_get_bids(FloxBookHandle book, double* prices_out,
                            double* qtys_out, uint32_t max_levels);
uint32_t flox_book_get_asks(FloxBookHandle book, double* prices_out,
                            double* qtys_out, uint32_t max_levels);
```

---

## L3 Order book

```c
FloxL3BookHandle flox_l3_book_create(void);
void             flox_l3_book_destroy(FloxL3BookHandle book);

int32_t flox_l3_book_add_order(FloxL3BookHandle book,
                               uint64_t order_id, double price,
                               double quantity, uint8_t side);
int32_t flox_l3_book_remove_order(FloxL3BookHandle book, uint64_t order_id);
int32_t flox_l3_book_modify_order(FloxL3BookHandle book,
                                  uint64_t order_id, double new_qty);

uint8_t flox_l3_book_best_bid(FloxL3BookHandle book, double* price_out);
uint8_t flox_l3_book_best_ask(FloxL3BookHandle book, double* price_out);
double  flox_l3_book_bid_at_price(FloxL3BookHandle book, double price);
double  flox_l3_book_ask_at_price(FloxL3BookHandle book, double price);
```

---

## Composite book

Aggregates books across multiple exchanges per symbol.

```c
FloxCompositeBookHandle flox_composite_book_create(void);
void                    flox_composite_book_destroy(FloxCompositeBookHandle book);

uint8_t flox_composite_book_best_bid(FloxCompositeBookHandle book, uint32_t symbol,
                                     double* price_out, double* qty_out);
uint8_t flox_composite_book_best_ask(FloxCompositeBookHandle book, uint32_t symbol,
                                     double* price_out, double* qty_out);
uint8_t flox_composite_book_has_arb(FloxCompositeBookHandle book, uint32_t symbol);
void    flox_composite_book_mark_stale(FloxCompositeBookHandle book,
                                       uint32_t exchange, uint32_t symbol);
void    flox_composite_book_check_staleness(FloxCompositeBookHandle book,
                                            int64_t now_ns, int64_t threshold_ns);
```

---

## Position tracker

FIFO/average cost position tracking.

```c
FloxPositionTrackerHandle flox_position_tracker_create(uint8_t cost_basis); // 0 = FIFO
void                      flox_position_tracker_destroy(FloxPositionTrackerHandle tracker);

void   flox_position_tracker_on_fill(FloxPositionTrackerHandle tracker,
                                     uint32_t symbol, uint8_t side,
                                     double price, double quantity);
double flox_position_tracker_position(FloxPositionTrackerHandle tracker, uint32_t symbol);
double flox_position_tracker_avg_entry(FloxPositionTrackerHandle tracker, uint32_t symbol);
double flox_position_tracker_realized_pnl(FloxPositionTrackerHandle tracker, uint32_t symbol);
double flox_position_tracker_total_pnl(FloxPositionTrackerHandle tracker);
```

---

## Position group

Tracks individual named positions (open/partial-close/close).

```c
FloxPositionGroupHandle flox_position_group_create(void);
void                    flox_position_group_destroy(FloxPositionGroupHandle tracker);

uint64_t flox_position_group_open(FloxPositionGroupHandle tracker,
                                  uint64_t order_id, uint32_t symbol,
                                  uint8_t side, double price, double qty);
void     flox_position_group_close(FloxPositionGroupHandle tracker,
                                   uint64_t position_id, double exit_price);
void     flox_position_group_partial_close(FloxPositionGroupHandle tracker,
                                           uint64_t position_id,
                                           double qty, double exit_price);

double   flox_position_group_net_position(FloxPositionGroupHandle tracker, uint32_t symbol);
double   flox_position_group_realized_pnl(FloxPositionGroupHandle tracker, uint32_t symbol);
double   flox_position_group_total_pnl(FloxPositionGroupHandle tracker);
uint32_t flox_position_group_open_count(FloxPositionGroupHandle tracker, uint32_t symbol);
void     flox_position_group_prune(FloxPositionGroupHandle tracker);
```

---

## Order tracker

Tracks submitted/filled/canceled orders.

```c
FloxOrderTrackerHandle flox_order_tracker_create(void);
void                   flox_order_tracker_destroy(FloxOrderTrackerHandle tracker);

uint8_t  flox_order_tracker_on_submitted(FloxOrderTrackerHandle tracker,
                                         uint64_t order_id, uint32_t symbol,
                                         uint8_t side, double price, double qty);
uint8_t  flox_order_tracker_on_filled(FloxOrderTrackerHandle tracker,
                                      uint64_t order_id, double fill_qty);
uint8_t  flox_order_tracker_on_canceled(FloxOrderTrackerHandle tracker, uint64_t order_id);
uint8_t  flox_order_tracker_is_active(FloxOrderTrackerHandle tracker, uint64_t order_id);
uint32_t flox_order_tracker_active_count(FloxOrderTrackerHandle tracker);
uint32_t flox_order_tracker_total_count(FloxOrderTrackerHandle tracker);
void     flox_order_tracker_prune(FloxOrderTrackerHandle tracker);
```

---

## Volume profile

```c
FloxVolumeProfileHandle flox_volume_profile_create(double tick_size);
void                    flox_volume_profile_destroy(FloxVolumeProfileHandle profile);

void     flox_volume_profile_add_trade(FloxVolumeProfileHandle profile,
                                       double price, double quantity, uint8_t is_buy);
double   flox_volume_profile_poc(FloxVolumeProfileHandle profile);
double   flox_volume_profile_vah(FloxVolumeProfileHandle profile);
double   flox_volume_profile_val(FloxVolumeProfileHandle profile);
double   flox_volume_profile_total_volume(FloxVolumeProfileHandle profile);
double   flox_volume_profile_total_delta(FloxVolumeProfileHandle profile);
uint32_t flox_volume_profile_num_levels(FloxVolumeProfileHandle profile);
void     flox_volume_profile_clear(FloxVolumeProfileHandle profile);
```

---

## Market profile

Tracks TPO-style market profile with initial balance.

```c
FloxMarketProfileHandle flox_market_profile_create(double tick_size,
                                                   uint32_t period_minutes,
                                                   int64_t session_start_ns);
void flox_market_profile_destroy(FloxMarketProfileHandle profile);

void     flox_market_profile_add_trade(FloxMarketProfileHandle profile,
                                       int64_t timestamp_ns, double price,
                                       double qty, uint8_t is_buy);
double   flox_market_profile_poc(FloxMarketProfileHandle profile);
double   flox_market_profile_vah(FloxMarketProfileHandle profile);
double   flox_market_profile_val(FloxMarketProfileHandle profile);
double   flox_market_profile_ib_high(FloxMarketProfileHandle profile);
double   flox_market_profile_ib_low(FloxMarketProfileHandle profile);
uint8_t  flox_market_profile_is_poor_high(FloxMarketProfileHandle profile);
uint8_t  flox_market_profile_is_poor_low(FloxMarketProfileHandle profile);
uint32_t flox_market_profile_num_levels(FloxMarketProfileHandle profile);
void     flox_market_profile_clear(FloxMarketProfileHandle profile);
```

---

## Footprint

Per-price buy/sell delta at bar resolution.

```c
FloxFootprintHandle flox_footprint_create(double tick_size);
void                flox_footprint_destroy(FloxFootprintHandle footprint);

void     flox_footprint_add_trade(FloxFootprintHandle footprint,
                                  double price, double quantity, uint8_t is_buy);
double   flox_footprint_total_delta(FloxFootprintHandle footprint);
double   flox_footprint_total_volume(FloxFootprintHandle footprint);
uint32_t flox_footprint_num_levels(FloxFootprintHandle footprint);
void     flox_footprint_clear(FloxFootprintHandle footprint);
```

---

## Statistics

| Function | Returns | Description |
|----------|---------|-------------|
| `flox_stat_correlation(x, y, len)` | `double` | Pearson correlation |
| `flox_stat_profit_factor(pnl, len)` | `double` | Gross profit / gross loss |
| `flox_stat_win_rate(pnl, len)` | `double` | Winning trade ratio |
| `flox_stat_permutation_test(g1, l1, g2, l2, n)` | `double` | Two-sample permutation p-value |
| `flox_stat_bootstrap_ci(data, len, conf, n, &lo, &med, &hi)` | `void` | Bootstrap confidence interval |

---

## Data writer

Writes trades to binary log segments.

```c
FloxDataWriterHandle flox_data_writer_create(const char* output_dir,
                                             uint64_t max_segment_mb,
                                             uint8_t exchange_id);
void flox_data_writer_destroy(FloxDataWriterHandle writer);

uint8_t flox_data_writer_write_trade(FloxDataWriterHandle writer,
                                     int64_t exchange_ts_ns, int64_t recv_ts_ns,
                                     double price, double qty,
                                     uint64_t trade_id, uint32_t symbol_id, uint8_t side);
void flox_data_writer_flush(FloxDataWriterHandle writer);
void flox_data_writer_close(FloxDataWriterHandle writer);
void flox_data_writer_stats_p(FloxDataWriterHandle writer, void* out); // → FloxWriterStats
```

---

## Data reader

Reads binary log segments.

```c
FloxDataReaderHandle flox_data_reader_create(const char* data_dir);
FloxDataReaderHandle flox_data_reader_create_filtered(const char* data_dir,
                                                       int64_t from_ns, int64_t to_ns,
                                                       const uint32_t* symbols,
                                                       uint32_t num_symbols);
void flox_data_reader_destroy(FloxDataReaderHandle reader);

uint64_t flox_data_reader_count(FloxDataReaderHandle reader);
void     flox_data_reader_summary_p(FloxDataReaderHandle reader, void* out); // → FloxDatasetSummary
void     flox_data_reader_stats_p(FloxDataReaderHandle reader, void* out);   // → FloxReaderStats

// Returns number of trades read. If trades_out is NULL, counts only.
uint64_t flox_data_reader_read_trades(FloxDataReaderHandle reader,
                                      FloxTradeRecord* trades_out, uint64_t max_trades);
```

`FloxTradeRecord` fields: `exchange_ts_ns`, `recv_ts_ns`, `price_raw`, `qty_raw`, `trade_id`, `symbol_id`, `side`.

---

## Data recorder

Records live market data to disk.

```c
FloxDataRecorderHandle flox_data_recorder_create(const char* output_dir,
                                                  const char* exchange_name,
                                                  uint64_t max_segment_mb);
void flox_data_recorder_destroy(FloxDataRecorderHandle recorder);

void    flox_data_recorder_add_symbol(FloxDataRecorderHandle recorder,
                                      uint32_t symbol_id, const char* name,
                                      const char* base, const char* quote,
                                      int8_t price_precision, int8_t qty_precision);
void    flox_data_recorder_start(FloxDataRecorderHandle recorder);
void    flox_data_recorder_stop(FloxDataRecorderHandle recorder);
void    flox_data_recorder_flush(FloxDataRecorderHandle recorder);
uint8_t flox_data_recorder_is_recording(FloxDataRecorderHandle recorder);
```

---

## Segment operations

```c
// Quick validate/merge
uint8_t flox_segment_validate(const char* path);
uint8_t flox_segment_merge(const char* input_dir, const char* output_path);

// Full API (results written to out pointer, see struct definitions in header)
void flox_segment_merge_full_p(const char* input_paths, size_t num_paths,
                                const char* output_dir, const char* output_name,
                                uint8_t sort, void* out);         // → FloxMergeResult
void flox_segment_merge_dir_p(const char* input_dir,
                               const char* output_dir, void* out); // → FloxMergeResult
void flox_segment_split_p(const char* input_path, const char* output_dir,
                           uint8_t mode, int64_t time_interval_ns,
                           uint64_t events_per_file, void* out);   // → FloxSplitResult
void flox_segment_export_p(const char* input_path, const char* output_path,
                            uint8_t format, int64_t from_ns, int64_t to_ns,
                            const uint32_t* symbols, uint32_t num_symbols,
                            void* out);                            // → FloxExportResult

uint8_t  flox_segment_recompress(const char* input_path, const char* output_path,
                                 uint8_t compression);
uint64_t flox_segment_extract_symbols(const char* input_path, const char* output_path,
                                      const uint32_t* symbols, uint32_t num_symbols);
uint64_t flox_segment_extract_time_range(const char* input_path, const char* output_path,
                                         int64_t from_ns, int64_t to_ns);

// Validation
void flox_segment_validate_full_p(const char* path, uint8_t verify_crc,
                                   uint8_t verify_timestamps, void* out); // → FloxSegmentValidation
void flox_dataset_validate_p(const char* data_dir, void* out);            // → FloxDatasetValidation
```

---

## Partitioner

Splits a dataset into time or event-count partitions for parallel backtesting.

```c
FloxPartitionerHandle flox_partitioner_create(const char* data_dir);
void                  flox_partitioner_destroy(FloxPartitionerHandle partitioner);

// All return number of partitions. If partitions_out is NULL, counts only.
uint32_t flox_partitioner_by_time(FloxPartitionerHandle p, uint32_t num_partitions,
                                   int64_t warmup_ns,
                                   FloxPartition* partitions_out, uint32_t max);
uint32_t flox_partitioner_by_duration(FloxPartitionerHandle p, int64_t duration_ns,
                                       int64_t warmup_ns,
                                       FloxPartition* partitions_out, uint32_t max);
uint32_t flox_partitioner_by_calendar(FloxPartitionerHandle p, uint8_t unit,
                                       int64_t warmup_ns,
                                       FloxPartition* partitions_out, uint32_t max);
uint32_t flox_partitioner_by_symbol(FloxPartitionerHandle p, uint32_t num_partitions,
                                     FloxPartition* partitions_out, uint32_t max);
uint32_t flox_partitioner_per_symbol(FloxPartitionerHandle p,
                                      FloxPartition* partitions_out, uint32_t max);
uint32_t flox_partitioner_by_event_count(FloxPartitionerHandle p, uint32_t num_partitions,
                                          FloxPartition* partitions_out, uint32_t max);
```

`FloxPartition` fields: `partition_id`, `from_ns`, `to_ns`, `warmup_from_ns`, `estimated_events`, `estimated_bytes`.

---

## Fixed-point conversion

```c
int64_t flox_price_from_double(double value);
double  flox_price_to_double(int64_t raw);
int64_t flox_quantity_from_double(double value);
double  flox_quantity_to_double(int64_t raw);
```

Scale factor is 1e8 for both price and quantity.
