# Python Bindings

## Strategy API

Event-driven live trading and backtesting using `Strategy`, `Runner`, and `BacktestRunner`.

### Build

```bash
cmake -B build \
  -DFLOX_BUILD_PYTHON=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Requires: Python 3.10+, pybind11 (`pip install pybind11`).

The module builds at `build/python/flox_py.cpython-*.so`.

### Symbols

```python
import flox_py as flox

registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)
# btc.id, btc.name, btc.exchange, btc.tick_size
# int(btc) → 1 — works as int everywhere
```

### Writing a Strategy

```python
class SMAcross(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = flox.SMA(10)
        self.slow = flox.SMA(30)

    def on_start(self): ...
    def on_stop(self): ...

    def on_trade(self, ctx, trade):
        f = self.fast.update(trade.price)
        s = self.slow.update(trade.price)
        if f is None or s is None:
            return
        if f > s and ctx.is_flat():
            self.market_buy(0.01)
        elif f < s and ctx.is_long():
            self.close_position()

    def on_book_update(self, ctx): ...
```

Order methods: `market_buy(qty)`, `market_sell(qty)`, `limit_buy(price, qty)`, `limit_sell(price, qty)`, `stop_market(side, trigger, qty)`, `close_position()`. All accept an optional `symbol` argument; without it the first registered symbol is used.

### Live Runner

```python
def on_signal(sig):
    # sig.side, sig.order_type, sig.quantity, sig.price, sig.order_id
    send_to_exchange(sig)

runner = flox.Runner(registry, on_signal)                  # synchronous
runner = flox.Runner(registry, on_signal, threaded=True)   # Disruptor background thread

runner.add_strategy(SMAcross([btc]))
runner.start()

# Inject market data (from your feed):
runner.on_trade(btc, price, qty, is_buy, ts_ns)
runner.on_book_snapshot(btc, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)

runner.stop()
```

### Backtest

```python
bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(SMAcross([btc]))

stats = bt.run_csv("btcusdt_1m.csv", "BTCUSDT")
print(stats["return_pct"], stats["sharpe"], stats["max_drawdown_pct"])
```

The constructor takes `(registry, fee_rate=0.0004, initial_capital=100000.0)` — nothing else. Flat fee rate, no funding, no liquidation, no rate limits, no queue position. Useful for an indicator sanity check; not enough for a decision about real capital.

`run_csv` reads OHLCV **bars**: one header line, then `timestamp,open,high,low,close,volume`. Only the timestamp and close columns are used; each bar is replayed as one trade, so `on_trade` fires and `on_bar` does not. Timestamps below `1e12` are treated as seconds and scaled to nanoseconds. Other entry points: `run_bars(...)` (full OHLC, fires `on_bar`), `run_ohlcv(ts, close, symbol)`, `run_tape(path)` and `run_tapes(paths)` for `.floxlog` tapes.

Returns a plain dict. Keys: `total_trades`, `winning_trades`, `losing_trades`, `initial_capital`, `final_capital`, `total_pnl`, `total_fees`, `net_pnl`, `gross_profit`, `gross_loss`, `max_drawdown`, `max_drawdown_pct`, `win_rate`, `profit_factor`, `sharpe`, `sortino`, `return_pct`.

Hooks attach after construction: `set_executor(executor)` (a subclass of `flox.Executor`), `set_risk_manager`, `set_kill_switch`, `set_order_validator`, `set_pnl_tracker`, `add_execution_listener`.

#### Venue physics

`VenueStack` is a separate, self-contained venue simulation — it is not an argument to `BacktestRunner` and does not attach to one.

```python
stack = flox.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)

acct = stack.account()
liq = stack.liquidation()
fees = stack.fees()
funding = stack.funding()
exec_ = stack.executor()
```

One call wires the cross-margin account, MM tiers and ADL, the VIP fee schedule (bound to the account, so realized notional moves the tier), funding settlement on the venue's interval, rate limits, and a venue-availability hook. Other factories: `bybit_linear`, `okx_swap`, `deribit`, plus `VenueStack.from_venue(name, account_id, equity)`. Non-canonical venues go through [`flox.assemble_custom_venue(...)`](../how-to/realistic-backtest.md#fully-custom-venue).

You drive the returned subsystems directly — `acct.open_position(...)`, `liq.on_marks(...)`, `fees.record_fill(...)`, `funding.tick(...)`, `exec_.submit_order(...)`. See `docs/examples/python_realistic_backtest.py`.

Full pattern and pieces: [Realistic backtest in one call](../how-to/realistic-backtest.md), [Cross-margin accounts](../how-to/cross-margin.md), [Liquidation and ADL](../how-to/liquidation-and-adl.md).

### Paper trading

Same strategy class, live feed, simulated fills. `PaperBroker` lives in `flox_py.paper`. It constructs its own `SimulatedExecutor` and its own `Runner`; you attach strategies to `broker.runner` and tee market data in through `broker.observe_trade(...)`.

```python
from flox_py.paper import PaperBroker

broker = PaperBroker(registry=registry)
broker.runner.add_strategy(SMAcross([btc]))
broker.start()

# Feed live trades from your data source (websocket, ccxt.pro, etc.)
broker.observe_trade(btc, price, qty, is_buy, ts_ns=ts_ns)

broker.stop()
print(broker.fills_list(), broker.stats)
```

`PaperBroker` is a dataclass; `on_signal=` is an optional user callback that fires *after* the simulator has accepted a signal, not a routing method. Slippage is tunable via `set_default_slippage(model, **params)` / `set_symbol_slippage(symbol_id, model, **params)`.

See [Paper trading](../how-to/paper-trading.md) for the full feed-wiring pattern.

### Live

`CcxtBroker` lives in `flox_py.ccxt` and routes orders through a [ccxt.pro](https://github.com/ccxt/ccxt) exchange. It is async and owns its own registry and runner — construct it from an exchange *id*, not an exchange object. The strategy class is unchanged.

```python
import asyncio
from flox_py.ccxt import CcxtBroker

async def main():
    broker = CcxtBroker("binance", api_key=..., secret=..., sandbox=True)
    async with broker:
        btc = await broker.add_symbol("BTC/USDT")
        broker.add_strategy(SMAcross([btc]))
        await broker.run(streams=("trades", "orders"))

asyncio.run(main())
```

One strategy class runs backtest, paper, and live. See [Connect FLOX to a CCXT exchange](../how-to/ccxt-adapter.md) and `docs/examples/python_ccxt_live.py`.

---

## Vectorised Engine

`flox.Engine` runs a backtest against a pre-built signal list instead of an event-driven `Strategy`. Bars are loaded once and can be re-run against many signal sets.

### Quick Start

```python
import flox_py as flox

engine = flox.Engine(initial_capital=100_000, fee_rate=0.0001)
engine.load_csv("btcusdt_1m.csv", symbol="BTCUSDT")

signals = flox.SignalBuilder()
signals.buy(1704067200_000000000, 0.5, symbol="BTCUSDT")
signals.sell(1704068400_000000000, 0.5, symbol="BTCUSDT")

stats = engine.run(signals)
print(f"PnL: {stats.net_pnl:.2f}, Sharpe: {stats.sharpe:.4f}")
```

### Loading Bar Data

| Method | Input |
|--------|-------|
| `load_csv(path, symbol="")` | OHLCV CSV; symbol inferred from the filename when omitted |
| `load_ohlcv(data, symbol="")` | dict with `ts` (or `timestamp`) plus `open`, `high`, `low`, `close`, `volume` arrays |
| `load_df(df, symbol="")` | pandas DataFrame; timestamp taken from `ts`/`timestamp`/`open_time`/`time`, else the index |

Call any of them once per symbol. `resample(symbol, target, interval)` derives a coarser series from a loaded one. Read-back accessors: `symbols`, `bar_count(symbol="")`, `ts`, `open`, `high`, `low`, `close`, `volume`.

### Creating Signals

`flox.SignalBuilder` accumulates signals in call order:

| Method | Arguments |
|--------|-----------|
| `buy` | `ts`, `qty`, `symbol=""` |
| `sell` | `ts`, `qty`, `symbol=""` |
| `limit_buy` | `ts`, `price`, `qty`, `symbol=""` |
| `limit_sell` | `ts`, `price`, `qty`, `symbol=""` |

`len(builder)` gives the count; `clear()` resets it. An empty `symbol` falls back to `run`'s `default_symbol`.

### Run

```python
stats = engine.run(signals, default_symbol=0)
```

Returns a `flox.Stats` object. Fields are readable as attributes or via `stats["key"]`; `stats.to_dict()` returns the whole set.

| Key | Description |
|-----|-------------|
| `total_trades` | Round-trip trade count |
| `net_pnl` | Gross PnL minus all fees |
| `total_fees` | Total execution fees |
| `sharpe` | Annualized Sharpe ratio |
| `sortino` | Annualized Sortino ratio |
| `calmar` | Calmar ratio |
| `max_drawdown` | Peak-to-trough drawdown |
| `max_drawdown_pct` | Drawdown as percentage |
| `win_rate` | Winning trade fraction |
| `profit_factor` | Gross profit / gross loss |
| `return_pct` | Net return percentage |

`Engine` exposes a single `run`; there is no batch or multi-threaded entry point. For parameter sweeps see [Grid Search](../how-to/grid-search.md).

## See Also

- [Python API Reference](../reference/python/index.md) — complete Python API documentation
- [Realistic backtest in one call](../how-to/realistic-backtest.md) — venue stack
- [Cross-margin accounts](../how-to/cross-margin.md) — shared equity across positions
- [Paper trading](../how-to/paper-trading.md) — same strategy class against a live feed
- [Connect FLOX to a CCXT exchange](../how-to/ccxt-adapter.md) — promote to live
- [Inspect a tape and run in the replay viewer](../how-to/replay-viewer.md)
- [Control engine over MCP](../how-to/mcp-control-plane.md) — scoped AI control
- [Grid Search](../how-to/grid-search.md) — parameter optimization
