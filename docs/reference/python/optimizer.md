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

engine = flox.Engine(initial_capital=100_000)
engine.load_bars_df(timestamps, opens, highs, lows, closes, volumes)

# Run strategy
signals = my_strategy(closes)
base_stats = engine.run(signals)
base_pnl = base_stats['net_pnl']

# Monte Carlo permutation test
rng = np.random.default_rng(42)
shuffled_sets = []
for _ in range(1000):
    idx = rng.permutation(len(signals))
    shuffled = signals[idx]
    shuffled_sets.append(shuffled)

results = engine.run_batch(shuffled_sets)
random_pnls = np.array([r['net_pnl'] for r in results])

# Statistical significance
p_value = np.mean(random_pnls >= base_pnl)
print(f"Strategy PnL: {base_pnl:.2f}, p-value: {p_value:.4f}")

# Confidence interval on trade PnLs
trade_pnls = flox.trade_pnl(signal_long, signal_short, log_returns)
lo, med, hi = flox.bootstrap_ci(trade_pnls)
print(f"Trade PnL: {med:.4f} [{lo:.4f}, {hi:.4f}]")

# Parameter sensitivity
param_sharpes = []
for period in range(5, 50):
    sigs = ma_strategy(closes, period)
    stats = engine.run(sigs)
    param_sharpes.append(stats['sharpe'])

r = flox.correlation(
    np.arange(5, 50, dtype=np.float64),
    np.array(param_sharpes),
)
print(f"Period-Sharpe correlation: {r:.4f}")
```
