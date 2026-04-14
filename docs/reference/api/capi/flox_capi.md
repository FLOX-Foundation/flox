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

## Conversion Helpers

| Function | Description |
|----------|-------------|
| `flox_price_from_double(value)` | `double` -> `int64_t` |
| `flox_price_to_double(raw)` | `int64_t` -> `double` |
| `flox_quantity_from_double(value)` | `double` -> `int64_t` |
| `flox_quantity_to_double(raw)` | `int64_t` -> `double` |
