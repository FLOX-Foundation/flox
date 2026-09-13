---
code: E_IND_001
title: Indicator period must be positive
severity: error
since: 0.8.1
---

# E_IND_001 — Indicator period must be positive

A rolling-window indicator (`sma`, `rma`, `bollinger`, `cci`, `vwap`, and the
`SMA` / `RMA` / `Bollinger` / `CCI` classes) was constructed or called with a
period of `0`. There is no window to average over with a zero-length period,
so the call is rejected instead of silently returning an all-NaN array.

## How to fix

Pass a period of `1` or more:

=== "Python"
    ```python
    import flox

    # OK
    flox.sma(prices, period=20)

    # Raises E_IND_001
    # flox.sma(prices, period=0)
    ```

## Common causes

- A period sourced from a config file or CLI argument that was left unset
  and defaulted to `0`.
- A parameter-search / optimizer bound that includes `0` in its range for a
  period argument.
- An off-by-one when deriving a period from another value (e.g.
  `fast_period - 1` when `fast_period` is `1`).
