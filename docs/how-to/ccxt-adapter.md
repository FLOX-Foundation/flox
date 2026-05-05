# Connect FLOX to a CCXT exchange

`flox_py.ccxt.CcxtBroker` wraps a [ccxt.pro](https://github.com/ccxt/ccxt/wiki/ccxt.pro) exchange. Market data flows in (trades, L2 books, order updates) and orders flow out (`self.market_buy(...)`, `self.limit_sell(...)`, `self.stop_market(...)` from inside the strategy translate into `create_*_order` calls on the exchange).

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

`add_symbol` calls `exchange.load_markets()` once and reads tick size from `markets[sym]["precision"]["price"]`. `add_strategy` attaches to the broker's internal `Runner`. The runner's signal callback is wired to `_handle_signal`, which dispatches to a per-order-type method (`_place_market`, `_place_limit`, `_place_stop_market`, ...). Calling `self.market_buy(...)` inside the strategy ends up calling `exchange.create_market_buy_order(...)`.

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

Stop, take-profit, and trailing parameter shapes differ across exchanges. The broker uses ccxt's unified-API defaults. If your exchange wants a different params dict (Binance futures uses `triggerPrice` instead of `stopPrice`, OKX uses `triggerPx`), subclass and override `_place_stop_market`, `_place_trailing`, etc.

Order types outside the table dispatch to `on_error(sym, UnsupportedOrderType(...))`. The exchange call is skipped.

## Position reconciliation

`run(reconcile=True)` (default) calls `fetch_balance()` and `fetch_positions()` before streams start. For each non-zero position on a registered symbol the broker calls `strategy.on_initial_position(ccxt_sym, qty, avg_price)`. If the strategy has no such method the position is just logged.

`fetch_positions` is a derivatives concept. Spot exchanges raise `NotSupported`; the broker swallows it and proceeds with an empty position list.

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

Instantiate one broker per exchange. A strategy attached to one broker only sees that exchange's market data; symbol ids are scoped to the broker that registered them.

## Runnable example

A complete script lives at [`docs/examples/python_ccxt_live.py`](../examples/python-ccxt-live.md): SMA(10/30) on `BTC/USDT` against Binance's public WebSocket. Runs in dry-run mode by default; set `FLOX_LIVE=1` plus `BINANCE_API_KEY` / `BINANCE_SECRET` to actually place sandbox orders.
