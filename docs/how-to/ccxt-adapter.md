# Connect FLOX to a CCXT exchange

`flox_py.ccxt.CcxtFeed` bridges any [ccxt.pro](https://github.com/ccxt/ccxt/wiki/ccxt.pro) WebSocket exchange (100+ supported) into a `flox.Runner` — trades and L2 book updates flow into your strategy without you writing the WebSocket plumbing.

## Install

```bash
pip install flox-py "ccxt[pro]"
```

## Quick start

```python
import asyncio
import ccxt.pro as ccxtpro
import flox_py as flox
from flox_py.ccxt import CcxtFeed

# 1. Register symbols.
registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)
eth = registry.add_symbol("binance", "ETHUSDT", tick_size=0.01)

# 2. Define a strategy and runner.
class MyStrategy(flox.Strategy):
    def on_trade(self, ctx, trade):
        # Your logic
        pass

def on_signal(sig):
    print(f"signal: {sig}")
    # In production: route to exchange via ccxt or your gateway.

runner = flox.Runner(registry, on_signal)
runner.add_strategy(MyStrategy([btc, eth]))
runner.start()

# 3. Wire CCXT into the runner.
exchange = ccxtpro.binance()  # public market data — no API key needed
feed = CcxtFeed(
    exchange=exchange,
    runner=runner,
    symbols={"BTC/USDT": btc, "ETH/USDT": eth},
    streams=("trades", "book"),  # subscribe to both, or one
    book_depth=20,
)

try:
    asyncio.run(feed.run())
except KeyboardInterrupt:
    pass
finally:
    runner.stop()
    asyncio.run(exchange.close())
```

## Order routing

`CcxtFeed` only handles **inbound** market data. Order placement is up to your signal handler — split keeps inbound (FLOX-controlled) and outbound (broker-controlled) paths clean:

```python
def on_signal(sig):
    if sig.side == "buy":
        asyncio.create_task(
            exchange.create_market_buy_order("BTC/USDT", sig.quantity)
        )
    elif sig.side == "sell":
        asyncio.create_task(
            exchange.create_market_sell_order("BTC/USDT", sig.quantity)
        )
```

For multi-exchange routing, instantiate a separate `CcxtFeed` per exchange.

## API reference

### `CcxtFeed(exchange, runner, symbols, *, streams=("trades","book"), book_depth=20, on_error=None)`

| Argument     | Type                          | Meaning |
|--------------|-------------------------------|---------|
| `exchange`   | ccxt.pro exchange instance    | Pre-configured (API keys if you'll trade) |
| `runner`     | `flox_py.Runner`              | Started before `feed.run()` |
| `symbols`    | `Mapping[str, Symbol]`        | `{ccxt_symbol: flox_symbol}`. CCXT format like `"BTC/USDT"` or `"BTC/USDT:USDT"` |
| `streams`    | `Iterable[str]`               | Subset of `{"trades", "book"}` |
| `book_depth` | `int`                         | L2 depth (number of bid/ask levels). Default 20 |
| `on_error`   | `Callable[[str, BaseException], None]` | Per-stream error handler. Default: log via `logging`. Pass `None` to silence |

### `await feed.run()`

Runs all configured streams concurrently. Returns when every stream task is cancelled. Typically called via `asyncio.run(feed.run())`; clean shutdown comes from `KeyboardInterrupt` or external cancellation.

### `await feed.stop()`

Cancels all in-flight stream tasks. Idempotent.

## Reconnection / errors

`watch_trades` and `watch_order_book` are wrapped in retry loops with a 1-second backoff. On exception, `on_error(symbol, exc)` fires (logged by default) and the loop reconnects. There's no max retry — disconnects are treated as transient.

For production strategies, monitor disconnects via your `on_error` callback and decide on a stop policy (e.g. flatten positions if disconnected for > 30 seconds).
