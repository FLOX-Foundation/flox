# Strategy Class

Event-driven strategy base class for Python. Mirrors C++ `flox::Strategy`.

## Class: `flox_py.Strategy`

### Constructor

```python
Strategy(symbols: list[int])
```

**Parameters:**

- `symbols` -- List of symbol IDs to subscribe to

### Overridable Callbacks

#### `on_trade(ctx: SymbolContext, trade: TradeData)`

Called on each trade event for subscribed symbols.

```python
def on_trade(self, ctx, trade):
    if trade.price > 100.0:
        self.emit_market_buy(ctx.symbol_id, 1.0)
```

#### `on_book_update(ctx: SymbolContext)`

Called on each order book update.

#### `on_start()` / `on_stop()`

Lifecycle callbacks.

### Signal Emission

| Method | Returns | Description |
|--------|---------|-------------|
| `emit_market_buy(symbol, qty)` | `int` | Market buy order, returns order ID |
| `emit_market_sell(symbol, qty)` | `int` | Market sell order |
| `emit_limit_buy(symbol, price, qty)` | `int` | Limit buy order |
| `emit_limit_sell(symbol, price, qty)` | `int` | Limit sell order |
| `emit_cancel(order_id)` | `None` | Cancel order |
| `emit_cancel_all(symbol)` | `None` | Cancel all orders for symbol |
| `emit_modify(order_id, price, qty)` | `None` | Modify existing order |
| `emit_stop_market(symbol, side, trigger, qty)` | `int` | Stop market order |
| `emit_stop_limit(symbol, side, trigger, limit, qty)` | `int` | Stop limit order |
| `emit_take_profit_market(symbol, side, trigger, qty)` | `int` | Take profit market |
| `emit_take_profit_limit(symbol, side, trigger, limit, qty)` | `int` | Take profit limit |
| `emit_trailing_stop(symbol, side, offset, qty)` | `int` | Trailing stop |
| `emit_trailing_stop_percent(symbol, side, bps, qty)` | `int` | Trailing stop (%) |
| `emit_close_position(symbol)` | `int` | Close position (reduce-only) |

### Context Queries

| Method | Returns | Description |
|--------|---------|-------------|
| `position(symbol)` | `float` | Current position quantity |
| `ctx(symbol)` | `SymbolContext` | Per-symbol context snapshot |
| `get_order_status(order_id)` | `int` | Order status (-1 if not found) |

## Class: `flox_py.SymbolContext`

| Property | Type | Description |
|----------|------|-------------|
| `symbol_id` | `int` | Symbol identifier |
| `position` | `float` | Current position |
| `last_trade_price` | `float` | Last trade price |
| `best_bid` | `float` | Best bid price |
| `best_ask` | `float` | Best ask price |
| `mid_price` | `float` | Mid price |
| `unrealized_pnl` | `float` | Unrealized P&L |
| `book_spread()` | `float` | Bid-ask spread |
| `is_long()` | `bool` | True if long |
| `is_short()` | `bool` | True if short |
| `is_flat()` | `bool` | True if no position |

## Class: `flox_py.TradeData`

| Property | Type | Description |
|----------|------|-------------|
| `symbol` | `int` | Symbol ID |
| `price` | `float` | Trade price |
| `quantity` | `float` | Trade quantity |
| `is_buy` | `bool` | Buy side |
| `timestamp_ns` | `int` | Timestamp (ns) |

## Example

```python
import flox_py as flox

class MomentumStrategy(flox.Strategy):
    def __init__(self, symbols, threshold=0.01):
        super().__init__(symbols)
        self.prev_price = 0.0
        self.threshold = threshold

    def on_trade(self, ctx, trade):
        if self.prev_price > 0:
            change = (trade.price - self.prev_price) / self.prev_price
            if change > self.threshold and ctx.is_flat():
                self.emit_market_buy(ctx.symbol_id, 1.0)
            elif change < -self.threshold and ctx.is_long():
                self.emit_close_position(ctx.symbol_id)
        self.prev_price = trade.price
```
