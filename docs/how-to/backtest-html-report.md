# Render a backtest as HTML

`flox_py.report.write_html` turns a `BacktestRunner` result into a self-contained HTML page with summary cards, equity curve, drawdown timeline, and a trades table. Inline SVG, no external assets — the file works offline and can be checked into a repo.

## In-process: from a strategy script

```python
import flox_py as flox
from flox_py.report import write_html

registry = flox.SymbolRegistry()
btc = registry.add_symbol("exchange", "BTCUSDT", tick_size=0.01)

class SMA(flox.Strategy):
    def __init__(self, syms):
        super().__init__(syms)
        self.fast = flox.SMA(10); self.slow = flox.SMA(30)
    def on_trade(self, ctx, t):
        f = self.fast.update(t.price); s = self.slow.update(t.price)
        if f is None or s is None: return
        if f > s and ctx.is_flat(): self.market_buy(0.01)
        elif f < s and ctx.is_flat(): self.market_sell(0.01)

bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(SMA([btc]))
stats = bt.run_csv("data.csv", symbol="BTCUSDT")

write_html(
    "report.html",
    stats=stats,
    equity_curve=bt.equity_curve(),
    trades=bt.trades(),
    title="My strategy",
    subtitle="SMA(10/30) on BTCUSDT",
)
```

`bt.equity_curve()` returns a dict of numpy arrays: `timestamp_ns`, `equity`, `drawdown_pct`. `bt.trades()` returns the closed trade table: `symbol`, `side` (0=long, 1=short), `entry_price`, `exit_price`, `quantity`, `pnl`, `fee`, `entry_time_ns`, `exit_time_ns`. Both raise `FloxError(E_RUN_002)` if called before any `run_*` method has completed.

## From the CLI

When the runner isn't in the same process — for example, you dumped the stats from a CI job to a JSON file — render from the command line:

```bash
flox report stats.json -o report.html
flox report stats.json -o report.html --equity equity.json --trades trades.json
```

`stats.json` must contain the dict returned by `BacktestRunner.run_csv()`. `--equity` and `--trades` are optional; without them the report shows summary cards only and the chart sections render an empty-state message.

## What the report contains

- **Summary cards** at the top: return, sharpe, max drawdown, trade count, win rate, profit factor, net PnL, fees, initial / final capital.
- **Equity curve** — equity vs time, with a horizontal line at initial capital.
- **Drawdown** — drawdown percent over the same time axis.
- **Trades** — the first 50 closed trades, with side, entry / exit price, quantity, pnl, fees. Win rows are green, lose rows are red.

The whole page is one HTML file. No CDN, no inline scripts beyond what the SVG needs, no telemetry.

## Other bindings

The same accessors exist on Node and Codon: `runner.equityCurve()` / `runner.trades()` (Node) and `runner.equity_curve()` / `runner.trades()` (Codon). The HTML rendering itself is Python-only — Node / Codon users can dump the arrays and shell out to `flox report`.
