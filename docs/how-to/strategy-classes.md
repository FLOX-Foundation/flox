# Strategy Classes (Python & Codon)

Flox provides event-driven Strategy base classes for both Python and Codon that
mirror the C++ `Strategy` class. This enables writing trading strategies in a
familiar Python-like syntax while leveraging the C++ execution engine.

## Architecture

```
C++ Strategy (fastest)
      |
  C API Layer (libflox_capi.so)
      |                |
Python Strategy    Codon Strategy
(pybind11)         (C FFI, compiled)
```

## Python Strategy

```python
import flox_py as flox

class SmaCrossover(flox.Strategy):
    def __init__(self, symbols, fast=10, slow=30):
        super().__init__(symbols)
        self.prices = []
        self.fast = fast
        self.slow = slow

    def on_trade(self, ctx, trade):
        self.prices.append(trade.price)
        if len(self.prices) < self.slow:
            return

        fast_sma = sum(self.prices[-self.fast:]) / self.fast
        slow_sma = sum(self.prices[-self.slow:]) / self.slow

        if fast_sma > slow_sma and ctx.is_flat():
            self.emit_market_buy(ctx.symbol_id, 1.0)
        elif fast_sma < slow_sma and ctx.is_long():
            self.emit_close_position(ctx.symbol_id)
```

## Codon Strategy

```python
from flox.strategy import Strategy
from flox.context import SymbolContext
from flox.types import TradeData
from flox.indicators import StreamingSMA

class SmaCrossover(Strategy):
    fast_sma: StreamingSMA
    slow_sma: StreamingSMA

    def __init__(self, symbols: List[int], fast: int = 10, slow: int = 30):
        super().__init__(symbols)
        self.fast_sma = StreamingSMA(fast)
        self.slow_sma = StreamingSMA(slow)

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        price = trade.price.to_double()
        fast = self.fast_sma.update(price)
        slow = self.slow_sma.update(price)
        if not self.slow_sma.ready:
            return

        sym = self._symbols[0]
        if fast > slow and ctx.is_flat():
            self.emit_market_buy(sym, 1.0)
        elif fast < slow and ctx.is_long():
            self.emit_close_position(sym)
```

## API Reference

### Overridable Callbacks

| Method | Description |
|--------|-------------|
| `on_trade(ctx, trade)` | Called on each trade event |
| `on_book_update(ctx)` | Called on each book update |
| `on_start()` | Called when strategy starts |
| `on_stop()` | Called when strategy stops |

### Signal Emission

| Method | Description |
|--------|-------------|
| `emit_market_buy(symbol, qty)` | Submit market buy order |
| `emit_market_sell(symbol, qty)` | Submit market sell order |
| `emit_limit_buy(symbol, price, qty)` | Submit limit buy order |
| `emit_limit_sell(symbol, price, qty)` | Submit limit sell order |
| `emit_cancel(order_id)` | Cancel an order |
| `emit_cancel_all(symbol)` | Cancel all orders for symbol |
| `emit_modify(order_id, price, qty)` | Modify existing order |
| `emit_stop_market(symbol, side, trigger, qty)` | Stop market order |
| `emit_stop_limit(symbol, side, trigger, limit, qty)` | Stop limit order |
| `emit_take_profit_market(symbol, side, trigger, qty)` | Take profit market |
| `emit_take_profit_limit(symbol, side, trigger, limit, qty)` | Take profit limit |
| `emit_trailing_stop(symbol, side, offset, qty)` | Trailing stop |
| `emit_trailing_stop_percent(symbol, side, bps, qty)` | Trailing stop (%) |
| `emit_close_position(symbol)` | Close entire position |

### Context Queries

| Method | Description |
|--------|-------------|
| `position(symbol)` | Current position as float |
| `ctx(symbol)` | Get SymbolContext for a symbol |
| `get_order_status(order_id)` | Order status (-1 if not found) |

### SymbolContext Properties

| Property | Description |
|----------|-------------|
| `symbol_id` | Symbol identifier |
| `position` | Current position quantity |
| `last_trade_price` | Last trade price |
| `best_bid` | Best bid price |
| `best_ask` | Best ask price |
| `mid_price` | Mid price |
| `book_spread()` | Bid-ask spread |
| `is_long()` / `is_short()` / `is_flat()` | Position state |

### TradeData Properties

| Property | Description |
|----------|-------------|
| `symbol` | Symbol ID |
| `price` | Trade price (float) |
| `quantity` | Trade quantity (float) |
| `is_buy` | Whether trade was a buy |
| `timestamp_ns` | Exchange timestamp in nanoseconds |

## C++ Equivalent

The Strategy classes mirror the C++ `flox::Strategy`:

| Python/Codon | C++ |
|-------------|-----|
| `on_trade(ctx, trade)` | `onSymbolTrade(SymbolContext&, TradeEvent&)` |
| `on_book_update(ctx)` | `onSymbolBook(SymbolContext&, BookUpdateEvent&)` |
| `on_start()` / `on_stop()` | `start()` / `stop()` |
| `emit_market_buy(sym, qty)` | `emitMarketBuy(sym, qty)` |
| `emit_market_sell(sym, qty)` | `emitMarketSell(sym, qty)` |
| `emit_limit_buy(sym, price, qty)` | `emitLimitBuy(sym, price, qty)` |
| `emit_limit_sell(sym, price, qty)` | `emitLimitSell(sym, price, qty)` |
| `emit_cancel(order_id)` | `emitCancel(orderId)` |
| `emit_cancel_all(sym)` | `emitCancelAll(sym)` |
| `emit_modify(order_id, price, qty)` | `emitModify(orderId, price, qty)` |
| `emit_stop_market(sym, side, trigger, qty)` | `emitStopMarket(sym, side, trigger, qty)` |
| `emit_stop_limit(sym, side, trigger, limit, qty)` | `emitStopLimit(sym, side, trigger, limit, qty)` |
| `emit_take_profit_market(sym, side, trigger, qty)` | `emitTakeProfitMarket(sym, side, trigger, qty)` |
| `emit_take_profit_limit(sym, side, trigger, limit, qty)` | `emitTakeProfitLimit(sym, side, trigger, limit, qty)` |
| `emit_trailing_stop(sym, side, offset, qty)` | `emitTrailingStop(sym, side, offset, qty)` |
| `emit_trailing_stop_percent(sym, side, bps, qty)` | `emitTrailingStopPercent(sym, side, bps, qty)` |
| `emit_close_position(sym)` | `emitClosePosition(sym)` |
| `position(sym)` | `position(sym)` |
| `ctx(sym)` | `ctx(sym)` |
| `get_order_status(order_id)` | `getOrderStatus(orderId)` |
