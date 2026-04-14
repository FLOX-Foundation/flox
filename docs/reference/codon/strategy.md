# Strategy

::: flox.strategy.Strategy

Base class for Codon event-driven strategies. Mirrors C++ `flox::Strategy`.

## Class: `Strategy`

### Constructor

```python
Strategy(symbols: List[int])
```

**Parameters:**

- `symbols` -- List of symbol IDs to subscribe to

### Overridable Callbacks

#### `on_trade(ctx, trade)`

Called on each trade event for subscribed symbols.

```python
def on_trade(self, ctx: SymbolContext, trade: TradeData):
    price = trade.price.to_double()
    # strategy logic here
```

#### `on_book_update(ctx)`

Called on each order book update for subscribed symbols.

```python
def on_book_update(self, ctx: SymbolContext):
    spread = ctx.book_spread()
    # strategy logic here
```

#### `on_start()` / `on_stop()`

Lifecycle callbacks.

### Signal Emission

#### `emit_market_buy(symbol, qty) -> int`

Submit a market buy order. Returns order ID.

#### `emit_market_sell(symbol, qty) -> int`

Submit a market sell order. Returns order ID.

#### `emit_limit_buy(symbol, price, qty) -> int`

Submit a limit buy order. Returns order ID.

#### `emit_limit_sell(symbol, price, qty) -> int`

Submit a limit sell order. Returns order ID.

#### `emit_cancel(order_id)`

Cancel an order by ID.

#### `emit_cancel_all(symbol)`

Cancel all orders for a symbol.

#### `emit_modify(order_id, new_price, new_qty)`

Modify an existing order's price and quantity.

#### `emit_stop_market(symbol, side, trigger, qty) -> int`

Submit a stop market order. `side`: 0=BUY, 1=SELL.

#### `emit_stop_limit(symbol, side, trigger, limit_price, qty) -> int`

Submit a stop limit order.

#### `emit_take_profit_market(symbol, side, trigger, qty) -> int`

Submit a take profit market order.

#### `emit_take_profit_limit(symbol, side, trigger, limit_price, qty) -> int`

Submit a take profit limit order.

#### `emit_trailing_stop(symbol, side, offset, qty) -> int`

Submit a trailing stop order.

#### `emit_trailing_stop_percent(symbol, side, callback_bps, qty) -> int`

Submit a trailing stop with percentage callback. `callback_bps`: 100 = 1%.

#### `emit_limit_buy_tif(symbol, price, qty, tif) -> int`

Submit a limit buy with TimeInForce. `tif`: 0=GTC, 1=IOC, 2=FOK, 4=POST_ONLY.

#### `emit_limit_sell_tif(symbol, price, qty, tif) -> int`

Submit a limit sell with TimeInForce.

#### `emit_close_position(symbol) -> int`

Close entire position with a reduce-only market order.

### Context Queries

#### `position(symbol=None) -> float`

Current position quantity. If `symbol` is None, uses the first subscribed symbol.

#### `ctx(symbol=None) -> SymbolContext`

Get a `SymbolContext` for querying per-symbol state.

#### `get_order_status(order_id) -> int`

Get order status. Returns -1 if not found.

### Properties

#### `primary_symbol -> int`

Returns the first symbol in the subscription list.

## Example

```python
from flox.strategy import Strategy
from flox.context import SymbolContext
from flox.types import TradeData
from flox.indicators import StreamingEMA

class EmaCrossover(Strategy):
    fast_ema: StreamingEMA
    slow_ema: StreamingEMA

    def __init__(self, symbols: List[int]):
        super().__init__(symbols)
        self.fast_ema = StreamingEMA(12)
        self.slow_ema = StreamingEMA(26)

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        price = trade.price.to_double()
        fast = self.fast_ema.update(price)
        slow = self.slow_ema.update(price)

        if not self.slow_ema.ready:
            return

        sym = self.primary_symbol
        if fast > slow and ctx.is_flat():
            self.emit_market_buy(sym, 1.0)
        elif fast < slow and ctx.is_long():
            self.emit_close_position(sym)
```
