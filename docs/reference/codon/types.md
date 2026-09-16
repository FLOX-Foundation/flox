# Types

Core data types for Codon strategies. All types mirror their C++ equivalents
and use fixed-point arithmetic (scale 1e8) internally.

## `Price`

Fixed-point price with 8 decimal places.

```python
p = Price.from_double(42000.50)
print(p.to_double())   # 42000.5
print(p.raw())          # 4200050000000
print(p.is_zero())      # False
```

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Price.from_double(value)` | `Price` | Create from float |
| `Price.from_raw(raw)` | `Price` | Create from raw int64 |
| `to_double()` | `float` | Convert to float |
| `raw()` | `int` | Get raw int64 value |
| `is_zero()` | `bool` | Check if zero |

Supports comparison operators: `==`, `<`, `>`, `<=`, `>=` and arithmetic: `+`, `-`.

## `Quantity`

Fixed-point quantity with 8 decimal places. Same API as `Price`.

```python
q = Quantity.from_double(1.5)
print(q.to_double())  # 1.5
```

## `TradeData`

Trade event data passed to `Strategy.on_trade()`.

### Fields

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `int` | Symbol ID |
| `price` | `Price` | Trade price |
| `quantity` | `Quantity` | Trade quantity |
| `is_buy` | `bool` | Whether trade was a buy |
| `symbol_name` | `str` | Symbol name, resolved from the registry |
| `timestamp_ns` | `int` | Exchange timestamp (nanoseconds) |

## `OrderEventData`

Order-lifecycle event for an order this strategy emitted. Passed to
`Strategy.on_fill()`, `on_order_update()`, `on_queue_position_change()` and
`on_market_position_change()`. Same fields, under the same names, as the
equivalents in the Python and Node bindings.

### Fields

| Field | Type | Description |
|-------|------|-------------|
| `order_id` | `int` | Order ID this event belongs to |
| `symbol` | `int` | Symbol ID |
| `symbol_name` | `str` | Symbol name, resolved from the registry |
| `side` | `str` | `"buy"` or `"sell"` |
| `order_type` | `int` | C++ `flox::OrderType` code, not the signal-type code |
| `status` | `int` | Order status code |
| `fill_qty` | `float` | Quantity filled by this event |
| `fill_price` | `float` | Price this event filled at |
| `exchange_ts_ns` | `int` | Exchange timestamp (nanoseconds) |
| `is_maker` | `bool` | Whether the fill was passive |
| `queue_ahead` | `float` | Quantity ahead in the queue. Backtest only |
| `queue_total` | `float` | Total quantity at the level. Backtest only |
| `market_position` | `str` | `"best"`, `"behind_best"`, `"mid_spread"`, `"level_empty"`, `"crossed"`, or `""` when the venue reports none |
| `distance_to_best_ticks` | `int` | Signed ticks from best on our side |

## Constants

### Side

| Constant | Value | Description |
|----------|-------|-------------|
| `BUY` | `0` | Buy side |
| `SELL` | `1` | Sell side |

### Order Type

| Constant | Value |
|----------|-------|
| `ORDER_MARKET` | `0` |
| `ORDER_LIMIT` | `1` |
| `ORDER_STOP_MARKET` | `2` |
| `ORDER_STOP_LIMIT` | `3` |
| `ORDER_TAKE_PROFIT_MARKET` | `4` |
| `ORDER_TAKE_PROFIT_LIMIT` | `5` |
| `ORDER_TRAILING_STOP` | `6` |

### Time in Force

| Constant | Value |
|----------|-------|
| `TIF_GTC` | `0` |
| `TIF_IOC` | `1` |
| `TIF_FOK` | `2` |
| `TIF_GTD` | `3` |
| `TIF_POST_ONLY` | `4` |

## `SymbolContext`

Per-symbol state passed to `Strategy.on_trade()` and `Strategy.on_book_update()`. Also accessible via `Strategy.ctx(symbol)`.

```python
from flox.context import SymbolContext

def on_trade(self, ctx: SymbolContext, trade: TradeData):
    if ctx.is_flat() and trade.price.to_double() > ctx.best_ask():
        self.emit_market_buy(ctx.symbol_id, 1.0)
```

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `symbol_id` | `int` | Numeric symbol ID |
| `symbol` | `str` | Symbol name |

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `position()` | `float` | Current position quantity |
| `position_raw()` | `int` | Current position (raw int64, scale 1e8) |
| `last_trade_price()` | `float` | Last trade price |
| `best_bid()` | `float` | Best bid price |
| `best_ask()` | `float` | Best ask price |
| `mid_price()` | `float` | Mid price |
| `book_spread()` | `float` | Bid-ask spread |
| `is_long()` | `bool` | Position > 0 |
| `is_short()` | `bool` | Position < 0 |
| `is_flat()` | `bool` | Position == 0 |

---

## Scale

All fixed-point types use `SCALE = 100_000_000` (1e8), matching the C++ `Decimal` template.
