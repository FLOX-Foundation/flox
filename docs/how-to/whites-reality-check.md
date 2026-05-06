# White's reality check

A bootstrap test for whether the **best** strategy among K candidates
beats zero, after correcting for the bias from picking the best.

The data-snooping problem: take 50 random strategies, keep the one
with the highest Sharpe, the marginal p-value looks great — but the
multiple-comparison penalty usually erases it. White (2000) handles
the penalty directly. Under the null that no strategy has positive
expected excess return, the test asks how likely the observed best
statistic is.

The implementation uses the [stationary bootstrap of Politis & Romano
(1994)][PR94] so the bootstrap samples preserve the autocorrelation
of the original return series.

[PR94]: https://www.jstor.org/stable/2290993

## Inputs

A 2D matrix of **excess returns** shaped `(num_strategies,
num_periods)`. Each row is one strategy's return series. Benchmark
adjustment is on the caller — pass raw returns to test against zero,
or `returns - benchmark` otherwise.

`num_bootstrap` is the resample count (default 10 000).
`avg_block_size` is the mean block length; `0` (default) picks
`sqrt(num_periods)`, a standard rule of thumb for return-series
autocorrelation.

## Python

```python
import flox_py as flox
import numpy as np

# rows = strategies, cols = periods. Pass excess returns.
returns = np.array([
    strat_a_excess_returns,   # length T
    strat_b_excess_returns,
    strat_c_excess_returns,
])

result = flox.whites_reality_check(
    returns,
    num_bootstrap=10_000,
    avg_block_size=0.0,   # auto = sqrt(T)
    seed=42,
)
print(f"best strategy index: {result['best_index']}")
print(f"observed statistic : {result['best_stat']:.4f}")
print(f"p-value            : {result['p_value']:.4f}")
```

A small p-value (say <5%) means the best strategy's edge survives
the multiple-comparison correction. A large one says the apparent
edge is likely a lucky pick.

## Node

```javascript
const flox = require('@flox-foundation/flox');

// Flat row-major: strategy 0 first, then strategy 1, ...
const K = 3;       // strategies
const T = 1000;    // periods
const returns = new Float64Array(K * T);
// fill with excess returns ...

const result = flox.whitesRealityCheck(returns, K, T, 10000, 0.0);
console.log(result);  // { p_value, best_stat, best_index }
```

## Codon

```python
from C import flox_stat_whites_reality_check(cobj, u64, u64, u32, f64,
                                              cobj, cobj, cobj) -> None

import numpy as np

returns = np.array([...], dtype=np.float64).reshape((K, T))
p = np.array([0.0])
stat = np.array([0.0])
idx = np.array([0], dtype=np.int32)
flox_stat_whites_reality_check(
    returns.ctypes.data, K, T, 10000, 0.0,
    p.ctypes.data, stat.ctypes.data, idx.ctypes.data,
)
print(p[0], stat[0], idx[0])
```

## Pairing with `GridSearch` and walk-forward

This is what to run after a grid search before keeping the
top-Sharpe cell.

```python
import numpy as np
import flox_py as flox

results = grid.run()

# Build (K, T) returns from per-cell return series. The exact source
# is up to your strategy: equity-curve log-returns, per-trade returns,
# or per-bar PnL.
returns = np.stack([cell["bar_returns"] for cell in results])

# Pass excess returns. Test against zero here; subtract a benchmark
# series before this line if needed.
out = flox.whites_reality_check(returns, num_bootstrap=10_000)
print(f"Best of {len(results)} cells: p={out['p_value']:.4f}")
```

Same pattern with [walk-forward](walk-forward.md): feed the
out-of-sample return series for each parameter setting and check
whether the best one is still significant after the sweep.

## Caveats

- The bootstrap recentres each strategy's mean to zero before
  building the bootstrap distribution. `best_stat` reflects the
  in-sample mean; the comparison happens against the bootstrap
  draws.
- The stationary bootstrap preserves short-range autocorrelation
  via block resampling, but does not address survivorship bias in
  the strategy pool or look-ahead bias in the data.
- The C ABI uses a fixed seed so the same inputs produce the same
  p-value. pybind11 exposes `seed=` for control; the NAPI binding
  inherits the C-side default (42).
- The penalty grows quickly with K. A 10 000-sample bootstrap is
  fine up to a few hundred candidates; above that, push
  `num_bootstrap` higher.
