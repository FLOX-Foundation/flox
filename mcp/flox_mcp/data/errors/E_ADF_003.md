---
code: E_ADF_003
title: ADF — NaN in input
severity: error
since: 0.5.7
---

# E_ADF_003 — ADF: NaN in input

The input series contains `NaN` values. ADF can't compute over missing
data; the result would be undefined.

## How to fix

Drop or fill NaN values before testing:

=== "Python"
    ```python
    import numpy as np

    clean = prices[~np.isnan(prices)]
    result = flox.adf(clean, max_lag=8, regression="c")

    # Or, if you need same-length output (e.g. rolling test):
    filled = np.where(np.isnan(prices), 0.0, prices)
    ```

## Common causes

- Indicator outputs producing leading NaNs (e.g. SMA's first `period-1`
  values).
- Returns computed via `prices[1:] - prices[:-1]` followed by reattaching
  to the original index, leaving NaN at position 0.
- Joins / merges that left unmatched rows.
