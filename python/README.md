# flox-py

Python bindings for [FLOX](https://github.com/FLOX-Foundation/flox), a modular framework for building trading systems with polyglot strategy bindings and AI-friendly developer tools. Same strategy code goes from backtest to live without a rewrite step.

## Install

```bash
pip install flox-py
```

## Scaffold a new project

```bash
flox new my-strategy                                   # research scaffold (default)
flox new my-bot --template=live                        # live trading via CcxtBroker
flox new my-indicators --template=indicator-library    # publishable indicator package
flox templates                                         # list templates
```

`flox new` ships with the wheel; see [`docs/how-to/flox-new.md`](https://flox-foundation.github.io/flox/how-to/flox-new/) for what each template lays down.

## Quick start

```python
import flox_py as flox

registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)

class SMACross(flox.Strategy):
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
runner.add_strategy(SMACross([btc]))
runner.start()
# feed market data:
# runner.on_trade(btc, price, qty, is_buy, ts_ns)
# runner.on_book_snapshot(btc, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)
runner.stop()
```

## Run a paper engine in one command

For tier-5/6 control (live order placement, position queries, kill switch over HTTP), `flox engine sim` boots a `Runner` + `SimulatedExecutor` + `ControlServer` + state-snapshot writer:

```bash
flox engine sim --strategy strategy.py --tape ./tape
```

Prints the engine URL and a copy-pasteable `flox-mcp init --engine-url URL --token T` command for wiring into AI tools.

## AI companion

`flox-mcp` is a Model Context Protocol server that gives coding agents (Claude Code, Cursor, Cline) grounded access to indicators, error codes, the C-API surface, and full-text doc search:

```bash
pip install flox-mcp
flox-mcp init           # writes ./.mcp.json for the current project
```

See the [flox-mcp README](https://github.com/FLOX-Foundation/flox/tree/main/mcp) for the tool list.

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

    def on_trade(self, ctx, trade): ...      # ctx: SymbolContext, trade: TradeData
    def on_book_update(self, ctx): ...
    def on_bar(self, ctx, bar): ...

    # Order-event hooks (fires on the strategy's own emitted orders).
    def on_fill(self, ctx, ev): ...           # status PARTIALLY_FILLED or FILLED
    def on_order_update(self, ctx, ev): ...   # every status change including fills
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

### OrderEvent (passed to `on_fill` / `on_order_update`)

| Property | Type | Description |
|----------|------|-------------|
| `order_id` | `int` | Engine order ID |
| `status` | `str` | `"FILLED"` / `"PARTIALLY_FILLED"` / `"REJECTED"` / `"CANCELED"` / ... |
| `side` | `str` | `"buy"` or `"sell"` |
| `fill_qty` | `float` | Last-fill quantity |
| `fill_price` | `float` | Last-fill price |
| `reject_reason` | `str \| None` | Set when status == REJECTED |

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
stats = bt.run_tape("./tape")              # replay a recorded `.floxlog` directory

equity = bt.equity_curve()   # numpy arrays: timestamp_ns, equity, drawdown_pct
trades = bt.trades()         # numpy arrays: per-trade detail

from flox_py.report import write_html
write_html("report.html", stats=stats, equity_curve=equity, trades=trades)
```

The same risk-gate stack as the live `Runner` plugs into the backtest. Reduce-only / flatten orders bypass the gate by design (so a tightening cap cannot strand a position):

```python
bt.set_risk_manager(my_risk_manager)        # IRiskManager: .allow(order)
bt.set_kill_switch(my_kill_switch)          # IKillSwitch: .check(order), .is_triggered()
bt.set_order_validator(my_validator)        # IOrderValidator: .validate(order, reason)
bt.set_pnl_tracker(my_pnl_tracker)          # IPnLTracker: .on_order_filled(order)
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
wf = flox.WalkForwardRunner(
    registry, fee_rate=0.0004, initial_capital=10_000,
    mode="anchored", train_size=180, test_size=30,
)
wf.set_strategy_factory(lambda fold_idx: MyStrategy([btc]))
folds = wf.run_csv("data.csv", "BTCUSDT")

grid = flox.GridSearch()
grid.add_axis([5, 10, 20])    # fast period
grid.add_axis([30, 50, 100])  # slow period
def factory(params):
    fast, slow = params
    bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
    bt.set_strategy(MyStrategy([btc]))  # configure with fast / slow as needed
    return bt.run_csv("data.csv", "BTCUSDT")
grid.set_factory(factory)
results = grid.run()    # list of {index, params, stats}
```

Render a heatmap with `flox_py.report.heatmap_html(...)` /
`write_heatmap(...)`. Run a multiple-comparison-aware significance
test with `flox.whites_reality_check(returns, num_bootstrap=10_000)`.

### MLflow

```python
from flox_py import mlflow as flox_mlflow

flox_mlflow.log_backtest(
    stats, equity_curve=equity, trades=trades,
    params={"fast": 10, "slow": 30},
    run_name="sma-2025-01",
)
```

`mlflow` is optional — install with `pip install mlflow`.

## Modules

| Module | Description |
|--------|-------------|
| Strategy / Runner | Event-driven live and backtest strategies |
| Engine | Batch backtest engine (signal arrays, parallel runs) |
| Indicators | EMA, SMA, RSI, MACD, ATR, Bollinger, ADX, Stochastic, CCI, VWAP, CVD, Correlation, AutoCorrelation, and more |
| Aggregators | Time, tick, volume, range, renko, Heikin-Ashi bars |
| Order Books | N-level, L3, cross-exchange CompositeBookMatrix |
| Profiles | Footprint bars, volume profile, market profile |
| Positions | Position tracking with FIFO/LIFO/average cost basis |
| Replay | Binary log reader/writer, market data recorder |

All compute-heavy operations release the GIL for true parallelism.

Full API reference at [flox-foundation.github.io/flox/reference/python](https://flox-foundation.github.io/flox/reference/python/).
