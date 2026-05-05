---
code: E_ADF_004
title: ADF — singular / degenerate regression
severity: error
since: 0.5.7
---

# E_ADF_004 — ADF: singular / degenerate regression

The ADF regression matrix is numerically singular — one or more
regressors are linearly dependent. This usually means the input series
is constant, near-constant, or otherwise lacks variation.

## How to fix

Check the input variance and the lag specification:

=== "Python"
    ```python
    import numpy as np

    print(f"variance: {np.var(prices)}")
    print(f"unique values: {len(np.unique(prices))}")

    # Reduce lag count if the series is short relative to max_lag.
    result = flox.adf(prices, max_lag=4, regression="c")
    ```

## Common causes

- The series is constant (`var == 0`) or near-constant after rounding.
- The series has < N unique values where N is the regressor count.
- `max_lag` is too large for the available variation.
- Floating-point underflow in the augmented regression — try the same
  series scaled (e.g. multiply by 1000 and re-test).
