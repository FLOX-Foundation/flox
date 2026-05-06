---
code: E_LEN_003
title: Wrong array dimensionality
severity: error
since: 0.5.7
---

# E_LEN_003 — Wrong array dimensionality

A function received a NumPy array with the wrong number of dimensions.
The reality-check / bootstrap statistics need a 2D
`(num_strategies, num_periods)` matrix, but the call passed a 1D
series or some other shape.

## How to fix

Reshape the data so each strategy's return series sits in its own
row:

=== "Python"
    ```python
    # If you have one strategy and a single return series:
    returns = single_series.reshape(1, -1)
    out = flox.whites_reality_check(returns, num_bootstrap=10_000)

    # Multiple strategies — stack them:
    returns = np.stack([strat_a, strat_b, strat_c])
    out = flox.whites_reality_check(returns, num_bootstrap=10_000)
    ```

## Common causes

- Forgot to stack per-strategy series into a 2D array.
- Passed a `pandas.Series` instead of a 2D `DataFrame.to_numpy()`.
- Passed `np.array([...])` of an iterable of iterables that fell back
  to dtype=object instead of float64; check `.ndim` before calling.
