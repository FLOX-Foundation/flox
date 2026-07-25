# Optimizer

Statistical tools for strategy validation and parameter optimization.

## permutation_test()

Two-sample permutation test. Tests whether two groups have the same mean. Useful for comparing strategy returns against random shuffles.

```python
p_value = flox.permutation_test(group1, group2, num_permutations=10000)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `group1` | `float64[]` | — | First sample |
| `group2` | `float64[]` | — | Second sample |
| `num_permutations` | `int` | `10000` | Number of permutation iterations |

**Returns:** `float` — p-value (probability of observing the actual difference by chance).

```python
# Test if strategy returns are significantly different from random
strategy_returns = np.array([0.01, 0.02, -0.005, 0.015, ...])
random_returns = np.array([0.001, -0.003, 0.002, -0.001, ...])

p = flox.permutation_test(strategy_returns, random_returns)
print(f"p-value: {p:.4f}")
if p < 0.05:
    print("Strategy returns are statistically significant")
```

---

## correlation()

Pearson correlation coefficient between two arrays.

```python
r = flox.correlation(x, y)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `x` | `float64[]` | First variable |
| `y` | `float64[]` | Second variable (same length as x) |

**Returns:** `float` — correlation coefficient in [-1, 1].

```python
# Check parameter sensitivity
param_values = np.array([10, 20, 30, 40, 50], dtype=np.float64)
sharpe_ratios = np.array([0.5, 1.2, 1.8, 1.5, 0.8])

r = flox.correlation(param_values, sharpe_ratios)
print(f"Correlation: {r:.4f}")
```

---

## bootstrap_ci()

Bootstrap confidence interval for the mean. Resamples the data with replacement to estimate uncertainty.

```python
lower, median, upper = flox.bootstrap_ci(data, confidence=0.95, num_samples=10000)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `data` | `float64[]` | — | Sample data (must be non-empty) |
| `confidence` | `float` | `0.95` | Confidence level (e.g., 0.95 for 95%) |
| `num_samples` | `int` | `10000` | Bootstrap iterations |

**Returns:** `tuple[float, float, float]` — `(lower, median, upper)` bounds.

```python
# Estimate confidence interval for strategy Sharpe ratio
trade_pnls = np.array([100, -50, 200, -30, 150, ...], dtype=np.float64)

lower, median, upper = flox.bootstrap_ci(trade_pnls, confidence=0.95)
print(f"Mean PnL: {median:.2f} [{lower:.2f}, {upper:.2f}] (95% CI)")
```

---

## Example: Full Validation Pipeline

```python
import numpy as np
import flox_py as flox

n = 2000
rng = np.random.default_rng(42)
ts = 1_700_000_000_000_000_000 + np.arange(n, dtype=np.int64) * 3_600_000_000_000
close = 30_000.0 + np.cumsum(rng.normal(0.0, 25.0, n))

engine = flox.Engine(initial_capital=100_000)
engine.load_ohlcv({
    "ts": ts,
    "open": close,
    "high": close + 5.0,
    "low": close - 5.0,
    "close": close,
    "volume": np.full(n, 1.0),
}, symbol="BTCUSDT")

closes = engine.close("BTCUSDT")
fast = flox.ema(closes, 10)
slow = flox.ema(closes, 30)


def build(indices, sides):
    """Turn (bar index, side) pairs into a SignalBuilder."""
    sb = flox.SignalBuilder()
    for i, side in zip(indices, sides):
        emit = sb.buy if side == 0 else sb.sell
        emit(int(ts[i]), 1.0, "BTCUSDT")
    return sb


# Base run: MA crossover
up = (fast[1:] > slow[1:]) & (fast[:-1] <= slow[:-1])
down = (fast[1:] < slow[1:]) & (fast[:-1] >= slow[:-1])
idx = np.flatnonzero(up | down) + 1
sides = np.where(up[idx - 1], 0, 1)

base_pnl = engine.run(build(idx, sides)).net_pnl

# Monte Carlo: same trade count and side sequence, random entry times
random_pnls = np.array([
    engine.run(build(np.sort(rng.choice(n, size=idx.size, replace=False)), sides)).net_pnl
    for _ in range(200)
])

p_value = float(np.mean(random_pnls >= base_pnl))
print(f"Strategy PnL: {base_pnl:.2f}, p-value: {p_value:.4f}")

# Confidence interval on per-trade PnLs
log_returns = np.diff(np.log(closes), prepend=np.log(closes[0]))
signal_long = (fast > slow).astype(np.int8)
signal_short = (fast < slow).astype(np.int8)

trade_pnls = flox.trade_pnl(signal_long, signal_short, log_returns)
lo, med, hi = flox.bootstrap_ci(trade_pnls)
print(f"Trade PnL: {med:.4f} [{lo:.4f}, {hi:.4f}]")

# Parameter sensitivity
periods = np.arange(5, 50)
param_sharpes = []
for period in periods:
    f = flox.ema(closes, int(period))
    p_up = (f[1:] > slow[1:]) & (f[:-1] <= slow[:-1])
    p_down = (f[1:] < slow[1:]) & (f[:-1] >= slow[:-1])
    p_idx = np.flatnonzero(p_up | p_down) + 1
    stats = engine.run(build(p_idx, np.where(p_up[p_idx - 1], 0, 1)))
    param_sharpes.append(stats.sharpe)

r = flox.correlation(periods.astype(np.float64), np.array(param_sharpes))
print(f"Period-Sharpe correlation: {r:.4f}")
```
