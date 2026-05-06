# flox-py

Python bindings for the [FLOX](https://github.com/FLOX-Foundation/flox) trading framework.

## Install

```bash
pip install flox-py
```

## Scaffold a new project

```bash
flox new my-strategy                            # research scaffold (default)
flox new my-bot --template=live                 # live trading via CcxtBroker
flox new my-indicators --template=indicator-library   # publishable indicator package
flox templates                                  # list templates
```

`flox new` ships with the wheel; see [`docs/how-to/flox-new.md`](https://flox-foundation.github.io/flox/how-to/flox-new/) for what each template lays down.

## Quick Start

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

def on_signal(sig):
    print(sig.side, sig.order_type, sig.quantity, sig.price)

runner = flox.Runner(registry, on_signal)
runner.add_strategy(SMAcross([btc]))
runner.start()
# feed market data:
# runner.on_trade(btc, price, qty, is_buy, ts_ns)
# runner.on_book_snapshot(btc, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)
runner.stop()
```

## Symbol and SymbolRegistry

```python
registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)

btc.id        # int, e.g. 1
btc.name      # "BTCUSDT"
btc.exchange  # "binance"
btc.tick_size # 0.01

int(btc)   # 1
print(btc) # Symbol(binance:BTCUSDT, id=1)
# Symbol objects work transparently as int anywhere an ID is expected
```

## Strategy

Subclass `flox.Strategy` and override the callbacks you need.

```python
class MyStrategy(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)   # symbols: list[Symbol | int]

    def on_start(self): ...
    def on_stop(self): ...

    def on_trade(self, ctx, trade):
        # ctx  -- SymbolContext
        # trade -- TradeData
        pass

    def on_book_update(self, ctx): ...
```

### Order emission (shorthand, uses first symbol by default)

| Method | Description |
|--------|-------------|
| `market_buy(qty, symbol=None)` | Market buy |
| `market_sell(qty, symbol=None)` | Market sell |
| `limit_buy(price, qty, symbol=None)` | Limit buy |
| `limit_sell(price, qty, symbol=None)` | Limit sell |
| `stop_market(side, trigger, qty, symbol=None)` | Stop market |
| `close_position(symbol=None)` | Close position (reduce-only) |

### SymbolContext

| Property | Type | Description |
|----------|------|-------------|
| `position` | `float` | Current position quantity |
| `last_trade_price` | `float` | Last trade price |
| `best_bid` | `float` | Best bid |
| `best_ask` | `float` | Best ask |
| `mid_price` | `float` | Mid price |
| `is_flat()` | `bool` | No position |
| `is_long()` | `bool` | Long position |
| `is_short()` | `bool` | Short position |

### TradeData

| Property | Type | Description |
|----------|------|-------------|
| `symbol` | `int` | Symbol ID |
| `price` | `float` | Trade price |
| `quantity` | `float` | Trade quantity |
| `is_buy` | `bool` | Buy-side aggressor |
| `timestamp_ns` | `int` | Timestamp (nanoseconds) |

## Runner

```python
runner = flox.Runner(registry, on_signal)                  # synchronous
runner = flox.Runner(registry, on_signal, threaded=True)   # Disruptor background thread

runner.add_strategy(strategy)
runner.start()
runner.on_trade(btc, price, qty, is_buy, ts_ns)
runner.on_book_snapshot(btc, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)
runner.stop()
```

The `on_signal` callback receives `Signal` objects:

| Property | Type | Description |
|----------|------|-------------|
| `side` | `str` | `"buy"` or `"sell"` |
| `quantity` | `float` | Order quantity |
| `price` | `float` | Limit price (0 for market) |
| `order_type` | `str` | `"market"`, `"limit"`, etc. |
| `order_id` | `int` | Internal order ID |

## BacktestRunner

```python
bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(MyStrategy([btc]))

stats = bt.run_csv("data.csv")             # auto-detects symbol from registry
stats = bt.run_csv("data.csv", "BTCUSDT")  # explicit symbol name

equity = bt.equity_curve()   # list of {timestamp_ns, equity, drawdown_pct}
trades = bt.trades()         # per-trade detail

from flox_py.report import write_html
write_html("report.html", stats=stats, equity_curve=equity, trades=trades)
```

### Stats dict

| Key | Description |
|-----|-------------|
| `return_pct` | Net return percentage |
| `net_pnl` | Net P&L after fees |
| `total_trades` | Round-trip trade count |
| `win_rate` | Winning trade fraction |
| `sharpe` | Annualized Sharpe ratio |
| `max_drawdown_pct` | Peak-to-trough drawdown (%) |

### Walk-forward and grid search

```python
wf = flox.WalkForwardRunner(registry, mode="anchored",
                            train_periods=180, test_periods=30)
folds = wf.run(strategy_factory, dataset_path="data.csv", symbol="BTCUSDT")

grid = flox.GridSearch(registry, factory=lambda p: build_strategy(*p))
grid.add_axis("fast", [5, 10, 20])
grid.add_axis("slow", [30, 50, 100])
results = grid.run("data.csv", symbol="BTCUSDT")
```

Render a heatmap with `flox_py.report.heatmap_html(...)` /
`write_heatmap(...)`. Run a multiple-comparison-aware significance
test with `flox.whites_reality_check(returns, num_bootstrap=10_000)`.

### MLflow

```python
from flox_py import mlflow as flox_mlflow

flox_mlflow.log_backtest(stats, equity_curve=equity, trades=trades,
                          params={"fast": 10, "slow": 30},
                          run_name="sma-2025-01")
```

`mlflow` is optional — install with `pip install mlflow`.

## Modules

| Module | Description |
|--------|-------------|
| Strategy / Runner | Event-driven live and backtest strategies |
| Engine | Batch backtest engine (signal arrays, parallel runs) |
| Indicators | EMA, SMA, RSI, MACD, ATR, Bollinger, ADX, Stochastic, CCI, VWAP, CVD, and more |
| Aggregators | Time, tick, volume, range, renko, Heikin-Ashi bars |
| Order Books | N-level, L3, cross-exchange CompositeBookMatrix |
| Profiles | Footprint bars, volume profile, market profile |
| Positions | Position tracking with FIFO/LIFO/average cost basis |
| Replay | Binary log reader/writer, market data recorder |

All compute-heavy operations release the GIL for true parallelism.

Full API reference at [flox-foundation.github.io/flox/reference/python](https://flox-foundation.github.io/flox/reference/python/).
