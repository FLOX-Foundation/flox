# C API Reference

The C API (`flox_capi.h`) provides a language-agnostic interface to the Flox engine.
It is the universal bridge used by both Python (via pybind11) and Codon (via C FFI).

## Header

```c
#include "flox/capi/flox_capi.h"
```

## Opaque Handles

```c
typedef void* FloxStrategyHandle;
typedef void* FloxRegistryHandle;
```

## Data Structures

### `FloxTradeData`

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `uint32_t` | Symbol ID |
| `price_raw` | `int64_t` | Price * 1e8 |
| `quantity_raw` | `int64_t` | Quantity * 1e8 |
| `is_buy` | `uint8_t` | 1 = buy, 0 = sell |
| `exchange_ts_ns` | `int64_t` | Exchange timestamp (ns) |

### `FloxBookSnapshot`

| Field | Type | Description |
|-------|------|-------------|
| `bid_price_raw` | `int64_t` | Best bid price * 1e8 |
| `ask_price_raw` | `int64_t` | Best ask price * 1e8 |
| `mid_raw` | `int64_t` | Mid price * 1e8 |
| `spread_raw` | `int64_t` | Spread * 1e8 |

### `FloxSymbolContext`

| Field | Type | Description |
|-------|------|-------------|
| `symbol_id` | `uint32_t` | Symbol ID |
| `position_raw` | `int64_t` | Position * 1e8 |
| `last_trade_price_raw` | `int64_t` | Last trade * 1e8 |
| `book` | `FloxBookSnapshot` | Book snapshot |

### `FloxStrategyCallbacks`

| Field | Type | Description |
|-------|------|-------------|
| `on_trade` | function pointer | Trade callback |
| `on_book` | function pointer | Book update callback |
| `on_start` | function pointer | Start callback |
| `on_stop` | function pointer | Stop callback |
| `user_data` | `void*` | Opaque user data |

## Registry Functions

### `flox_registry_create()`

Create a new symbol registry.

### `flox_registry_destroy(registry)`

Destroy a registry.

### `flox_registry_add_symbol(registry, exchange, name, tick_size)`

Register a symbol. Returns `SymbolId`.

## Strategy Functions

### `flox_strategy_create(id, symbols, num_symbols, registry, callbacks)`

Create a strategy with given callbacks. Returns `FloxStrategyHandle`.

### `flox_strategy_destroy(strategy)`

Destroy a strategy and free resources.

## Signal Emission

| Function | Returns | Description |
|----------|---------|-------------|
| `flox_emit_market_buy(s, sym, qty_raw)` | `OrderId` | Market buy |
| `flox_emit_market_sell(s, sym, qty_raw)` | `OrderId` | Market sell |
| `flox_emit_limit_buy(s, sym, px_raw, qty_raw)` | `OrderId` | Limit buy |
| `flox_emit_limit_sell(s, sym, px_raw, qty_raw)` | `OrderId` | Limit sell |
| `flox_emit_cancel(s, order_id)` | `void` | Cancel order |
| `flox_emit_cancel_all(s, sym)` | `void` | Cancel all |
| `flox_emit_modify(s, order_id, px_raw, qty_raw)` | `void` | Modify order |
| `flox_emit_stop_market(s, sym, side, trigger, qty)` | `OrderId` | Stop market |
| `flox_emit_stop_limit(s, sym, side, trigger, limit, qty)` | `OrderId` | Stop limit |
| `flox_emit_take_profit_market(s, sym, side, trigger, qty)` | `OrderId` | TP market |
| `flox_emit_take_profit_limit(s, sym, side, trigger, limit, qty)` | `OrderId` | TP limit |
| `flox_emit_trailing_stop(s, sym, side, offset, qty)` | `OrderId` | Trailing stop |
| `flox_emit_trailing_stop_percent(s, sym, side, bps, qty)` | `OrderId` | Trailing stop (%) |
| `flox_emit_limit_buy_tif(s, sym, px, qty, tif)` | `OrderId` | Limit buy + TIF |
| `flox_emit_limit_sell_tif(s, sym, px, qty, tif)` | `OrderId` | Limit sell + TIF |
| `flox_emit_close_position(s, sym)` | `OrderId` | Close position |

## Context Queries

| Function | Returns | Description |
|----------|---------|-------------|
| `flox_position_raw(s, sym)` | `int64_t` | Position * 1e8 |
| `flox_best_bid_raw(s, sym)` | `int64_t` | Best bid * 1e8 |
| `flox_best_ask_raw(s, sym)` | `int64_t` | Best ask * 1e8 |
| `flox_mid_price_raw(s, sym)` | `int64_t` | Mid price * 1e8 |
| `flox_get_order_status(s, order_id)` | `int32_t` | Order status (-1 = not found) |

## Indicator Functions

| Function | Description |
|----------|-------------|
| `flox_indicator_ema(input, len, period, output)` | EMA |
| `flox_indicator_sma(input, len, period, output)` | SMA |
| `flox_indicator_rsi(input, len, period, output)` | RSI |
| `flox_indicator_atr(high, low, close, len, period, output)` | ATR |
| `flox_indicator_macd(input, len, fast, slow, signal, ...)` | MACD |
| `flox_indicator_bollinger(input, len, period, mult, ...)` | Bollinger |

## Additional Indicators

| Function | Description |
|----------|-------------|
| `flox_indicator_rma(input, len, period, output)` | Wilder's MA |
| `flox_indicator_dema(input, len, period, output)` | Double EMA |
| `flox_indicator_tema(input, len, period, output)` | Triple EMA |
| `flox_indicator_kama(input, len, period, fast, slow, output)` | Kaufman Adaptive MA |
| `flox_indicator_slope(input, len, length, output)` | Linear slope |
| `flox_indicator_adx(high, low, close, len, period, adx, +di, -di)` | ADX |
| `flox_indicator_cci(high, low, close, len, period, output)` | CCI |
| `flox_indicator_stochastic(high, low, close, len, k, d, k_out, d_out)` | Stochastic |
| `flox_indicator_chop(high, low, close, len, period, output)` | Choppiness |
| `flox_indicator_obv(close, volume, len, output)` | On-Balance Volume |
| `flox_indicator_vwap(close, volume, len, window, output)` | Rolling VWAP |
| `flox_indicator_cvd(open, high, low, close, volume, len, output)` | Cumulative Volume Delta |

## Order Book

| Function | Description |
|----------|-------------|
| `flox_book_create(tick_size)` | Create NLevelOrderBook |
| `flox_book_destroy(book)` | Free book |
| `flox_book_apply_snapshot(book, bp, bq, bl, ap, aq, al)` | Full snapshot |
| `flox_book_apply_delta(book, bp, bq, bl, ap, aq, al)` | Incremental update |
| `flox_book_best_bid(book, &price)` | Best bid, returns 0 if empty |
| `flox_book_best_ask(book, &price)` | Best ask |
| `flox_book_mid(book, &price)` | Mid price |
| `flox_book_spread(book, &spread)` | Bid-ask spread |
| `flox_book_get_bids(book, prices, qtys, max)` | Get bid levels |
| `flox_book_get_asks(book, prices, qtys, max)` | Get ask levels |
| `flox_book_is_crossed(book)` | Check if crossed |
| `flox_book_clear(book)` | Clear all levels |

## Backtesting

| Function | Description |
|----------|-------------|
| `flox_executor_create()` | Create SimulatedExecutor |
| `flox_executor_submit_order(exec, id, side, price, qty, type, sym)` | Submit order |
| `flox_executor_on_bar(exec, symbol, close)` | Feed bar |
| `flox_executor_on_trade(exec, symbol, price, is_buy)` | Feed trade |
| `flox_executor_advance_clock(exec, timestamp_ns)` | Advance time |
| `flox_executor_fill_count(exec)` | Number of fills |
| `flox_executor_get_fills(exec, fills, max)` | Get fill array |

## Position Tracking

| Function | Description |
|----------|-------------|
| `flox_position_tracker_create(cost_basis)` | Create tracker (0=FIFO) |
| `flox_position_tracker_on_fill(t, symbol, side, price, qty)` | Record fill |
| `flox_position_tracker_position(t, symbol)` | Current position |
| `flox_position_tracker_avg_entry(t, symbol)` | Avg entry price |
| `flox_position_tracker_realized_pnl(t, symbol)` | Realized PnL |
| `flox_position_tracker_total_pnl(t)` | Total PnL |

## Statistics

| Function | Description |
|----------|-------------|
| `flox_stat_correlation(x, y, len)` | Pearson correlation |
| `flox_stat_profit_factor(pnl, len)` | Gross profit / gross loss |
| `flox_stat_win_rate(pnl, len)` | Winning trade ratio |
| `flox_stat_permutation_test(g1, l1, g2, l2, n)` | Two-sample p-value |
| `flox_stat_bootstrap_ci(data, len, conf, n, &lo, &med, &hi)` | Bootstrap CI |

## Conversion Helpers

| Function | Description |
|----------|-------------|
| `flox_price_from_double(value)` | `double` -> `int64_t` |
| `flox_price_to_double(raw)` | `int64_t` -> `double` |
| `flox_quantity_from_double(value)` | `double` -> `int64_t` |
| `flox_quantity_to_double(raw)` | `int64_t` -> `double` |
