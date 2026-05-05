---
code: E_ADF_002
title: ADF — input too short
severity: error
since: 0.5.7
---

# E_ADF_002 — ADF: input too short

The Augmented Dickey-Fuller test needs at least 4 observations, and
typically more depending on `max_lag` and the regression mode. The
input series didn't provide enough.

## How to fix

Use a longer input series or reduce `max_lag`:

=== "Python"
    ```python
    # OK: enough data for max_lag=4 with constant regression
    flox.adf(prices_500, max_lag=4, regression="c")

    # Avoid:
    # flox.adf(prices_5, max_lag=10, regression="ct")
    # → too short for max_lag=10 + 2 trend regressors
    ```

The minimum required length is approximately:

```
n_required = max_lag + n_regressors + 2
n_regressors = 1 (the lagged-level term)
             + (1 if regression in {"c", "ct"})
             + (1 if regression == "ct")
```

## Common causes

- Stationarity test on a per-day window of intraday returns where the
  window is too small.
- Forgetting that `max_lag` consumes observations from the front.
