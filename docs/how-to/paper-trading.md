# Paper trading with `PaperBroker`

`PaperBroker` runs your strategy against a live market data feed but routes orders to a `SimulatedExecutor` instead of a real exchange. The fill model is the same one backtests use: configurable slippage, queue position tracking, top-of-book or full-depth modes. A strategy that survives backtest gets to rehearse against current market conditions before any real capital is at stake.

## Install

```bash
pip install flox-py
```

If you also want to drive the broker off a live ccxt feed, add `[ccxt]`:

```bash
pip install "flox-py[ccxt]"
```

The broker itself does not depend on ccxt; it is feed-agnostic.

## Quick start

```python
import flox_py as flox
from flox_py.paper import PaperBroker

registry = flox.SymbolRegistry()
sym = registry.add_symbol("paper", "BTCUSDT", tick_size=0.01)

broker = PaperBroker(
    registry=registry,
    default_slippage_model="fixed_bps",
    default_slippage_params={"bps": 5.0},  # 5 bps against the taker
)

class MyStrategy(flox.Strategy):
    def on_trade(self, ctx, trade):
        if ctx.is_flat():
            self.market_buy(0.01)

broker.runner.add_strategy(MyStrategy([int(sym)]))
broker.start()

# In your feed loop, forward each trade:
for ts_ns, price, qty, is_buy in your_live_feed():
    broker.observe_trade(int(sym), price, qty, is_buy, ts_ns)

broker.stop()
print(broker.fills_list())
```

## How the wiring works

```
your feed ──► broker.observe_trade ──┬─► runner.on_trade ──► strategy
                                     │
                                     └─► sim.on_trade_qty (queue tracker)

strategy ──► self.market_buy() ──► broker._route_signal ──► sim.submit_order
                                                                  │
                                                                  ▼
                                                                fill
```

Every observed trade goes both into the runner (so strategies see it) and into the simulator (so its queue tracker stays current). When the strategy emits a signal, the broker's signal callback maps it to `SimulatedExecutor.submit_order`. No network call, no exchange. The fill comes back through `sim.fills_list()` once the simulator's matching engine settles it against subsequent observed trades.

## Slippage and queue model

`PaperBroker` exposes the same slippage / queue knobs the backtest engine uses. Defaults are:

- Slippage: `none`
- Queue model: whatever `SimulatedExecutor`'s default is (typically TOB)

Override per-broker at construction:

```python
broker = PaperBroker(
    registry=registry,
    default_slippage_model="volume_impact",
    default_slippage_params={"impact_coeff": 0.002},
)
```

Or per-symbol after construction:

```python
broker.set_symbol_slippage(int(sym), "fixed_bps", bps=10.0)
```

## What works, what does not

Routed to the simulator:

- `MARKET` (`market_buy`, `market_sell`)
- `LIMIT` (`limit_buy`, `limit_sell`)
- `CANCEL` for a known `order_id`
- `CANCEL_ALL` for a symbol

Not routed (yet). These signal types fall through to the user's optional `on_signal` callback so you can log or approximate them externally:

- `STOP_MARKET` / `STOP_LIMIT`
- `TAKE_PROFIT_MARKET` / `TAKE_PROFIT_LIMIT`
- `TRAILING_STOP` / `TRAILING_STOP_PERCENT`
- `CLOSE_POSITION`
- `MODIFY`

The reason is plain: `SimulatedExecutor.submit_order` accepts `market` / `limit` types only. Stops and trailing variants need a richer order machine. That gap closes as part of W14 follow-up; until then, keep paper-mode strategies on market and limit.

## Hooking in ccxt for a live feed

```python
import asyncio
import ccxt.pro as ccxt
from flox_py import SymbolRegistry
from flox_py.paper import PaperBroker

async def main():
    registry = SymbolRegistry()
    btc = registry.add_symbol("ccxt", "BTCUSDT", tick_size=0.01)

    broker = PaperBroker(
        registry=registry,
        default_slippage_model="fixed_bps",
        default_slippage_params={"bps": 5.0},
    )
    # ... add_strategy / start as in the quick start ...
    broker.start()

    exchange = ccxt.binance()
    try:
        while True:
            trades = await exchange.watch_trades("BTC/USDT")
            for t in trades:
                broker.observe_trade(
                    int(btc),
                    float(t["price"]),
                    float(t["amount"]),
                    t["side"] == "buy",
                    int(t["timestamp"]) * 1_000_000,
                )
    finally:
        broker.stop()
        await exchange.close()
        print(broker.fills_list())

asyncio.run(main())
```

This is the minimal pattern. Production setups will add error handling around `watch_trades`, persistent reconnection, and probably a separate task for `watch_order_book` plus `broker.observe_book_snapshot` so the simulator's queue tracker has top-of-book context.

## See also

* [Backtest with realistic fills](backtest-realistic-fills.md). The same `SimulatedExecutor` powering paper mode runs the backtest fills.
* [CCXT adapter](ccxt-adapter.md). The live order broker for the same flox pipeline; swap in `PaperBroker` to rehearse without sending live orders.
* [Record and replay tapes](tape-record.md). Recorded tapes can drive `PaperBroker` instead of a live feed for repeatable rehearsals.
