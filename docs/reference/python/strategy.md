# Strategy API

Event-driven strategy classes for Python. Mirrors C++ `flox::Strategy`.

## Class: `flox_py.Symbol`

Returned by `SymbolRegistry.add_symbol`. Works transparently as an `int` wherever a symbol ID is expected.

| Property | Type | Description |
|----------|------|-------------|
| `id` | `int` | Numeric symbol ID |
| `name` | `str` | Symbol name, e.g. `"BTCUSDT"` |
| `exchange` | `str` | Exchange name, e.g. `"binance"` |
| `tick_size` | `float` | Minimum price increment |

```python
int(btc)   # 1
print(btc) # Symbol(binance:BTCUSDT, id=1)
```

## Class: `flox_py.SymbolRegistry`

```python
registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)
```

| Method | Returns | Description |
|--------|---------|-------------|
| `add_symbol(exchange, name, tick_size)` | `Symbol` | Register a symbol and return its `Symbol` object |

## Class: `flox_py.Strategy`

### Constructor

```python
Strategy(symbols: list[Symbol | int])
```

`symbols` — list of `Symbol` objects or raw integer IDs to subscribe to.

### Overridable Callbacks

#### `on_trade(ctx: SymbolContext, trade: TradeData)`

Called on each trade event for subscribed symbols.

```python
def on_trade(self, ctx, trade):
    if ctx.is_flat():
        self.market_buy(0.01)
```

#### `on_book_update(ctx: SymbolContext)`

Called on each order book update.

#### `on_start()` / `on_stop()`

Lifecycle callbacks.

### Order Emission — Shorthand

These methods use the first registered symbol when `symbol` is omitted.

| Method | Description |
|--------|-------------|
| `market_buy(qty, symbol=None)` | Market buy |
| `market_sell(qty, symbol=None)` | Market sell |
| `limit_buy(price, qty, symbol=None)` | Limit buy |
| `limit_sell(price, qty, symbol=None)` | Limit sell |
| `stop_market(side, trigger, qty, symbol=None)` | Stop market |
| `close_position(symbol=None)` | Close position (reduce-only) |

### Order Emission — Explicit (emit_* variants)

| Method | Returns | Description |
|--------|---------|-------------|
| `emit_market_buy(symbol, qty)` | `int` | Market buy, returns order ID |
| `emit_market_sell(symbol, qty)` | `int` | Market sell |
| `emit_limit_buy(symbol, price, qty)` | `int` | Limit buy |
| `emit_limit_sell(symbol, price, qty)` | `int` | Limit sell |
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
| `is_buy` | `bool` | Buy-side aggressor |
| `timestamp_ns` | `int` | Timestamp (nanoseconds) |

## Class: `flox_py.Runner`

Feeds market data into strategies and routes emitted signals to a callback.

```python
runner = flox.Runner(registry, on_signal)                  # synchronous
runner = flox.Runner(registry, on_signal, threaded=True)   # Disruptor background thread

runner.add_strategy(strategy)
runner.start()
runner.on_trade(symbol, price, qty, is_buy, ts_ns)
runner.on_book_snapshot(symbol, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)
runner.stop()
```

| Method | Description |
|--------|-------------|
| `add_strategy(strategy)` | Register a strategy instance |
| `start()` | Start the runner |
| `stop()` | Stop the runner |
| `on_trade(symbol, price, qty, is_buy, ts_ns)` | Inject a trade event |
| `on_book_snapshot(symbol, bid_px, bid_qty, ask_px, ask_qty, ts_ns)` | Inject an order book snapshot |

`symbol` accepts a `Symbol` object or a raw `int`.

### Signal object

Passed to the `on_signal` callback.

| Property | Type | Description |
|----------|------|-------------|
| `side` | `str` | `"buy"` or `"sell"` |
| `quantity` | `float` | Order quantity |
| `price` | `float` | Limit price (0 for market) |
| `order_type` | `str` | `"market"`, `"limit"`, etc. |
| `order_id` | `int` | Internal order ID |

## Class: `flox_py.BacktestRunner`

Runs a strategy against historical CSV data.

```python
bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(strategy)

stats = bt.run_csv("data.csv")             # auto-detects symbol from registry
stats = bt.run_csv("data.csv", "BTCUSDT")  # explicit symbol name
```

| Method | Description |
|--------|-------------|
| `set_strategy(strategy)` | Set the strategy to backtest |
| `run_csv(path, symbol=None)` | Run backtest against a CSV file, returns stats dict |

### Stats dict keys

| Key | Description |
|-----|-------------|
| `return_pct` | Net return percentage |
| `net_pnl` | Net P&L after fees |
| `total_trades` | Round-trip trade count |
| `win_rate` | Winning trade fraction |
| `sharpe` | Annualized Sharpe ratio |
| `max_drawdown_pct` | Peak-to-trough drawdown (%) |

## Example

```python
import flox_py as flox

registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)

class SMAcross(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = flox.SMA(10)
        self.slow = flox.SMA(30)

    def on_trade(self, ctx, trade):
        f = self.fast.update(trade.price)
        s = self.slow.update(trade.price)
        if f is None or s is None:
            return
        if f > s and ctx.is_flat():
            self.market_buy(0.01)
        elif f < s and ctx.is_long():
            self.close_position()

# Live
def on_signal(sig):
    print(sig.side, sig.order_type, sig.quantity)

runner = flox.Runner(registry, on_signal)
runner.add_strategy(SMAcross([btc]))
runner.start()

# Backtest
bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(SMAcross([btc]))
stats = bt.run_csv("btcusdt_trades.csv", "BTCUSDT")
print(stats)
```
