# Python Bindings

## Strategy API

Event-driven live trading and backtesting using `Strategy`, `Runner`, and `BacktestRunner`.

### Build

```bash
cmake -B build \
  -DFLOX_ENABLE_PYTHON=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Requires: Python 3.9+, pybind11 (`pip install pybind11`).

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

stats = bt.run_csv("btcusdt_trades.csv")             # auto-detects symbol
stats = bt.run_csv("btcusdt_trades.csv", "BTCUSDT")  # explicit
print(stats["return_pct"], stats["sharpe"], stats["max_drawdown_pct"])
```

Stats dict keys: `return_pct`, `net_pnl`, `total_trades`, `win_rate`, `sharpe`, `max_drawdown_pct`.

---

## Batch Backtest Engine

Run backtests against pre-computed signal arrays. Execution runs in C++ with the GIL released and multi-threaded batch support.

### Build

```bash
cmake -B build \
  -DFLOX_ENABLE_BACKTEST=ON \
  -DFLOX_ENABLE_PYTHON=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Requires: Python 3.9+, pybind11, numpy (`pip install pybind11 numpy`).

### Quick Start

```python
import numpy as np
import flox_py as flox

engine = flox.Engine(initial_capital=100_000, fee_rate=0.0001)

# Load OHLCV data
engine.load_bars_df(timestamps, opens, highs, lows, closes, volumes)

# Create signals (all market orders)
signals = flox.make_signals(
    timestamps=np.array([1704067200000, 1704068400000], dtype=np.int64),
    sides=np.array([0, 1], dtype=np.uint8),  # 0=buy, 1=sell
    quantities=np.array([0.5, 0.5]),
)

stats = engine.run(signals)
print(f"PnL: {stats['net_pnl']:.2f}, Sharpe: {stats['sharpe']:.4f}")
```

### Loading Bar Data

Two options:

=== "From numpy arrays"

    ```python
    engine.load_bars_df(
        timestamps,  # int64, unix ms or ns (auto-detected)
        opens,       # float64
        highs,       # float64
        lows,        # float64
        closes,      # float64
        volumes,     # float64
    )
    ```

=== "From structured array"

    ```python
    bars = np.zeros(n, dtype=flox.PyBar)
    bars['timestamp_ns'] = ...
    bars['open_raw'] = (opens * 1e8).astype(np.int64)
    # ... etc
    engine.load_bars(bars)
    ```

Load once, then run as many backtests as needed against the same data.

### Creating Signals

`make_signals()` converts numpy arrays into a packed struct array:

```python
signals = flox.make_signals(
    timestamps,   # int64 — unix ms, us, or ns (auto-normalized)
    sides,        # uint8 — 0=buy, 1=sell
    quantities,   # float64 — position size
    prices,       # float64 — limit price (optional, default: market)
    types,        # uint8 — 0=market, 1=limit (optional, default: market)
)
```

For market-only strategies, `prices` and `types` can be omitted.

### Single Run

```python
stats = engine.run(signals, symbol=1)
```

Returns a dict with all backtest metrics:

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

### Batch Execution

Run N backtests in parallel using C++ threads:

```python
all_stats = engine.run_batch(
    [signals_1, signals_2, ..., signals_n],
    threads=0,   # 0 = use all cores
    symbol=1,
)
```

GIL released. Threads run independent copies, nothing shared.

#### Permutation Testing Example

```python
import flox_py as flox
import numpy as np

engine = flox.Engine(initial_capital=100_000, fee_rate=0.0001)
engine.load_bars_df(timestamps, opens, highs, lows, closes, volumes)

rng = np.random.default_rng(42)
signal_sets = []

for _ in range(1000):
    rets = np.diff(np.log(closes))
    rng.shuffle(rets)
    shuffled = closes[0] * np.exp(np.cumsum(np.concatenate(([0], rets))))
    sigs = my_strategy(shuffled, timestamps)
    signal_sets.append(sigs)

# 1000 backtests in ~50ms
results = engine.run_batch(signal_sets)
pnls = [r["net_pnl"] for r in results]
p_value = np.mean([p >= results[0]["net_pnl"] for p in pnls])
```

### Performance

Benchmarked on Apple M-series, 100K bars, MA cross strategy:

| Mode | Time | vs Python |
|------|------|-----------|
| Single run | 0.6ms | 33x |
| 1000 permutations | 53ms | 400x |
| Bar loading | 0.5ms | — |

## See Also

- [Python API Reference](../reference/python/index.md) — complete Python API documentation
- [Backtesting](backtest.md) — C++ backtest guide
- [Grid Search](grid-search.md) — parameter optimization
