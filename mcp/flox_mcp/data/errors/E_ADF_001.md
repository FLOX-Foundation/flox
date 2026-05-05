---
code: E_ADF_001
title: ADF — invalid regression mode
severity: error
since: 0.5.7
---

# E_ADF_001 — ADF: invalid regression mode

The Augmented Dickey-Fuller test received a `regression` argument that
isn't one of the three supported modes:

| Mode  | Regressors |
|-------|-----------|
| `"n"` | No constant, no trend |
| `"c"` | Constant (drift) — default |
| `"ct"`| Constant + linear trend |

## How to fix

=== "Python"
    ```python
    result = flox.adf(prices, max_lag=8, regression="c")
    ```

## Common causes

- Passing a long form like `"constant"` instead of `"c"`.
- Capitalisation: FLOX expects lowercase letters.
