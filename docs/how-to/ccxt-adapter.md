# Connect FLOX to a CCXT exchange

`flox_py.ccxt.CcxtBroker` wraps a [ccxt.pro](https://github.com/ccxt/ccxt/wiki/ccxt.pro) exchange and gives a strategy a single entry point for both market data (trades, L2 books, order updates) and order routing. The strategy's `self.market_buy(...)` / `self.limit_sell(...)` / `self.stop_market(...)` calls translate into real `create_*_order` calls on the underlying exchange.

## Install

```bash
pip install flox-py "ccxt[pro]"
```

## Quick start

```python
import asyncio
import flox_py as flox
from flox_py.ccxt import CcxtBroker


class SMA(flox.Strategy):
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
            self.market_buy(0.001)
        elif f < s and ctx.is_long():
            self.market_sell(0.001)

    def on_order_update(self, sym, status, filled, avg):
        print(f"  {sym} {status} filled={filled} avg={avg}")

    def on_initial_position(self, sym, qty, avg):
        print(f"  starting position on {sym}: qty={qty} avg={avg}")


async def main():
    async with CcxtBroker(
        "binance",
        api_key="...",       # leave None for public market data only
        secret="...",
        sandbox=True,
    ) as broker:
        btc = await broker.add_symbol("BTC/USDT")
        broker.add_strategy(SMA([btc]))
        await broker.run(streams=("trades", "book", "orders"))


asyncio.run(main())
```

`add_symbol` calls `exchange.load_markets()` once and reads tick size from the market metadata, so the strategy never hard-codes one. `add_strategy` attaches to the broker's internal `Runner`. The runner's signal callback is wired through to `_place_order` — calling `self.market_buy(...)` from inside the strategy results in a real ccxt order placement.

## Lifecycle

`async with CcxtBroker(...)` constructs the exchange. `await broker.run(...)` starts the runner, dispatches initial-position callbacks, spawns one asyncio task per (symbol × stream), and awaits until cancelled. On `__aexit__` the broker cancels stream tasks, stops the runner, and closes the exchange.

`broker.run(...)` blocks until cancelled (e.g. via `Ctrl+C` or an outer task cancel). To run alongside other coroutines, schedule it as a task and cancel it on shutdown.

## Streams

Pass any subset of `("trades", "book", "orders")` to `run(streams=...)`:

| Stream    | ccxt method               | FLOX target                                                    |
|-----------|---------------------------|----------------------------------------------------------------|
| `trades`  | `watch_trades(sym)`       | `runner.on_trade(sym_id, price, qty, is_buy, ts_ns)`           |
| `book`    | `watch_order_book(sym)`   | `runner.on_book_snapshot(sym_id, bids/asks)`                   |
| `orders`  | `watch_orders()`          | strategy's `on_order_update(ccxt_sym, status, filled, avg)`    |

Each yields a snapshot per call. The book stream forwards `book_depth` levels (default 20).

## Order routing

When a strategy emits a signal (via `self.market_buy(...)` etc.), the broker's signal callback maps it to ccxt:

| `Signal.order_type`         | ccxt call                                                                         |
|-----------------------------|-----------------------------------------------------------------------------------|
| `MARKET`                    | `create_market_<side>_order(sym, qty)`                                            |
| `LIMIT`                     | `create_limit_<side>_order(sym, qty, price)`                                      |
| `STOP_MARKET`               | `create_order(sym, "stop_market", side, qty, None, {"stopPrice": trigger})`       |
| `STOP_LIMIT`                | `create_order(sym, "stop_limit", side, qty, limit, {"stopPrice": trigger})`       |
| `TAKE_PROFIT_MARKET`        | `create_order(sym, "take_profit_market", side, qty, None, {"stopPrice": ...})`    |
| `TAKE_PROFIT_LIMIT`         | `create_order(sym, "take_profit_limit", side, qty, limit, {"stopPrice": ...})`    |
| `TRAILING_STOP`             | `create_order(sym, "trailing_stop_market", side, qty, None, {"trailingDelta": offset})` |
| `TRAILING_STOP_PERCENT`     | `create_order(sym, "trailing_stop_market", side, qty, None, {"callbackRate": bps/100})` |
| `CLOSE_POSITION`            | `create_order(sym, "market", side, qty, None, {"reduceOnly": True})`              |
| `CANCEL`                    | `cancel_order(ccxt_id, ccxt_sym)` using the tracked id from the original signal   |
| `CANCEL_ALL`                | `cancel_order` for every tracked id on the symbol                                 |
| `MODIFY`                    | `edit_order(ccxt_id, sym, "limit", side, new_qty, new_price)`                     |

Stop / take-profit / trailing parameters vary across exchanges. The broker uses ccxt's unified-API defaults; if a particular exchange needs a different params shape (e.g. Binance futures `stopPrice` vs. `triggerPrice`), subclass and override `_place_stop_market` / `_place_trailing` / etc.

Anything not in the table above goes through `on_error` as `UnsupportedOrderType` — the order is dropped, not silently routed somewhere unexpected.

## Position reconciliation

Before streams start, `run(reconcile=True)` (the default) calls `fetch_balance()` and `fetch_positions()`. For each non-zero position whose symbol is registered in the broker, the strategy's `on_initial_position(ccxt_sym, qty, avg_price)` callback is invoked. Strategies that don't define the method are skipped.

`fetch_positions` is a derivatives concept — for spot exchanges ccxt raises `NotSupported`, which the broker treats as "no positions" rather than fail.

## Backoff

Stream errors (network drops, exchange 5xx, etc.) are caught and retried with exponential backoff:

```python
broker = CcxtBroker(
    "binance",
    retry_initial_delay=1.0,    # first sleep after a failure
    retry_max_delay=60.0,       # cap
    retry_multiplier=2.0,       # delay *= multiplier on each consecutive fail
)
```

Delay starts at `retry_initial_delay`, multiplies on each consecutive failure, caps at `retry_max_delay`, and resets to the initial value after a successful yield.

## Subclassing per exchange

Override any of the `_place_*` methods to customise per-exchange params (Bybit `triggerBy`, OKX `tdMode`, Binance futures `closePosition`, etc.):

```python
class BybitBroker(CcxtBroker):
    async def _place_stop_market(self, sym, side, sig):
        await self.exchange.create_order(
            sym, "stop_market", side, self._qty(sig), None,
            {"stopPrice": self._trigger(sig), "triggerBy": "MarkPrice"},
        )
```

## Multi-exchange

Instantiate one broker per exchange. Strategies attached to one broker only see that exchange's market data — there is no cross-broker symbol id sharing.

## Runnable example

A complete script lives at [`docs/examples/python_ccxt_live.py`](../examples/python-ccxt-live.md): SMA(10/30) on `BTC/USDT` connected to Binance public WebSocket, no API key needed.
