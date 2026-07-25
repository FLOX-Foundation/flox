# Strategy classes

FLOX exposes the same event-driven Strategy model in every language. Override callbacks (`on_trade`, `on_book_update`, `on_bar`), emit orders through the strategy's helper methods, and the C++ engine handles bus dispatch, fills, and bookkeeping.

## Architecture

```
                   C++ Strategy (canonical)
                            |
                    C API (libflox_capi.so)
              ____________ | ____________
             |             |              |
   Python Strategy   Node Strategy   Codon Strategy
       (pybind11)        (N-API)      (C FFI, AOT)
```

All bindings dispatch through the same C++ `BridgeStrategy` callbacks, so behaviour is identical across languages.

## A simple strategy

10/30 SMA crossover, long-only.

=== "Python"

    ```python
    import flox_py as flox

    class SmaCrossover(flox.Strategy):
        def __init__(self, symbols, fast=10, slow=30):
            super().__init__(symbols)
            self.prices = []
            self.fast, self.slow = fast, slow

        def on_trade(self, ctx, trade):
            self.prices.append(trade.price)
            if len(self.prices) < self.slow:
                return
            f = sum(self.prices[-self.fast:]) / self.fast
            s = sum(self.prices[-self.slow:]) / self.slow
            if f > s and ctx.is_flat():
                self.market_buy(1.0)
            elif f < s and ctx.is_long():
                self.close_position()
    ```

=== "Node.js"

    ```javascript
    class SmaCrossover {
      constructor(symbols, fast = 10, slow = 30) {
        this.symbols = symbols;
        this.fast = fast; this.slow = slow;
        this.prices = [];
      }
      onTrade(ctx, trade, emit) {
        this.prices.push(trade.price);
        if (this.prices.length < this.slow) return;
        const f = this.prices.slice(-this.fast).reduce((a,b)=>a+b)/this.fast;
        const s = this.prices.slice(-this.slow).reduce((a,b)=>a+b)/this.slow;
        if (f > s && ctx.position === 0)        emit.marketBuy(1.0);
        else if (f < s && ctx.position > 0)     emit.closePosition();
      }
    }
    ```

=== "Codon"

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
            f = self.fast_sma.update(price)
            s = self.slow_sma.update(price)
            if not self.slow_sma.ready:
                return
            if f > s and self.position() == 0.0:
                self.market_buy(1.0)
            elif f < s and self.position() > 0.0:
                self.close_position()
    ```

=== "C++"

    ```cpp
    class SmaCrossover : public flox::Strategy {
      // ... see docs/how-to/backtest.md for the full C++ class
    };
    ```

## Overridable callbacks

| Callback | When it fires | Available |
|---|---|---|
| `on_trade(ctx, trade)` | Tick-level trade event | All bindings |
| `on_book_update(ctx)` | L2 book changed (bid/ask, mid) | All bindings |
| `on_bar(ctx, bar)` | Closed OHLC bar | All bindings (since [#123](https://github.com/FLOX-Foundation/flox/pull/123)) |
| `on_start()` / `on_stop()` | Lifecycle hooks | All bindings |
| `on_fill(ctx, event)` | Every fill on the strategy's own orders (`PARTIALLY_FILLED` / `FILLED`) | Python, Node.js, Codon |
| `on_order_update(ctx, event)` | Every order-lifecycle status change — `NEW` / `ACCEPTED` / `CANCELED` / `REJECTED` / `REPLACED` / `TRIGGERED` / `TRAILING_UPDATED`, fills included | Python, Node.js, Codon |
| `on_market_position_change(ctx, event)` | Resting limit order's categorical market position moved (`best` / `behind_best` / `mid_spread` / `level_empty` / `crossed`). Backtest only | Python, Node.js, Codon |
| `on_queue_position_change(ctx, event)` | Resting limit order's queue position moved with no other lifecycle transition; `event.queue_ahead` / `queue_total` carry the snapshot. Backtest only | Python, Node.js, Codon |

Node spells the last four `onFill`, `onOrderUpdate`,
`onMarketPositionChange`, `onQueuePositionChange`.

## Order emission helpers

Names differ slightly by language (snake_case vs camelCase) but the surface is the same.

| Action | Python | Node.js | Codon | C++ |
|---|---|---|---|---|
| Market buy | `market_buy(qty)` | `emit.marketBuy(qty)` | `market_buy(qty)` | `emitMarketBuy(sym, qty)` |
| Market sell | `market_sell(qty)` | `emit.marketSell(qty)` | `market_sell(qty)` | `emitMarketSell(sym, qty)` |
| Limit buy | `limit_buy(price, qty, tif="gtc")` | `emit.limitBuy(p, qty)` | `limit_buy(price, qty, tif="gtc")` | `emitLimitBuy(sym, p, qty[, tif])` |
| Limit sell | `limit_sell(price, qty, tif="gtc")` | `emit.limitSell(p, qty)` | `limit_sell(price, qty, tif="gtc")` | `emitLimitSell(sym, p, qty[, tif])` |
| Cancel | `cancel_order(id)` | `emit.cancel(id)` | `cancel_order(id)` | `emitCancel(id)` |
| Cancel all | `cancel_all_orders()` | — | `cancel_all_orders()` | `emitCancelAll(sym)` |
| Modify / replace | `modify_order(id, new_price, new_qty)` | — | `modify_order(id, new_price, new_qty)` | `emitModify(id, newPrice, newQty)` |
| Stop market | `stop_market(side, trigger, qty)` | — | `stop_market(side, trigger, qty)` | `emitStopMarket(sym, side, trigger, qty)` |
| Stop limit | `stop_limit(side, trigger, limit_price, qty)` | — | `stop_limit(side, trigger, limit_price, qty)` | `emitStopLimit(sym, side, trigger, limit, qty)` |
| Take profit market | `take_profit_market(side, trigger, qty)` | — | `take_profit_market(side, trigger, qty)` | `emitTakeProfitMarket(sym, side, trigger, qty)` |
| Take profit limit | `take_profit_limit(side, trigger, limit_price, qty)` | — | `take_profit_limit(side, trigger, limit_price, qty)` | `emitTakeProfitLimit(sym, side, trigger, limit, qty)` |
| Trailing stop | `trailing_stop(side, offset, qty)` | — | `trailing_stop(side, offset, qty)` | `emitTrailingStop(sym, side, offset, qty)` |
| Trailing stop (bps) | `trailing_stop_percent(side, callback_bps, qty)` | — | `trailing_stop_percent(side, callback_bps, qty)` | `emitTrailingStopPercent(sym, side, callbackBps, qty)` |
| Close position | `close_position()` | `emit.closePosition()` | `close_position()` | `emitClosePosition(sym)` |

In Python / Node.js / Codon `symbol` defaults to the first registered symbol when omitted.

The Node `emit` object carries only `marketBuy`, `marketSell`,
`limitBuy`, `limitSell`, `cancel`, `closePosition`,
`provideLiquidity`, and `withdrawLiquidity`. `cancelAll(symbol)` is a
`SimulatedExecutor` method, not an emit helper. `tif` is not
reachable from Node's `limitBuy` / `limitSell`.

`tif` accepts `gtc` (default), `ioc`, `fok`, or `post_only`;
anything else falls back to `gtc`.

## Context (`ctx`) properties

| Property | Type | Description |
|---|---|---|
| `position` | float | Current position quantity (signed) |
| `symbol_id` (Py/Codon) / `symbolId` (Node) | int | Numeric symbol id |
| `last_trade_price` / `lastTradePrice` | float | Most recent trade price |
| `best_bid` / `bestBid`, `best_ask` / `bestAsk` | float | Top-of-book |
| `mid_price` / `midPrice` | float | `(best_bid + best_ask) / 2` |
| `unrealized_pnl` (Py) | float | Unrealized PnL on the current position |
| `is_long()` / `is_short()` / `is_flat()` (Py/Codon) | bool | Position state |
| `book_spread()` (Py/Codon) | float | `best_ask - best_bid` |

Node's `SymbolContext` is the narrower set: `position`, `symbolId`,
`lastTradePrice`, `bestBid`, `bestAsk`, `midPrice`.

## C++ ↔ binding name map

For the canonical reference of every callback and emit helper, see the per-language API references:

- [Python reference](../reference/python/strategy.md)
- [Node.js reference](../reference/node/strategy.md)
- [Codon reference](../reference/codon/strategy.md)
- [C++ Strategy](../reference/api/strategy/strategy.md)
