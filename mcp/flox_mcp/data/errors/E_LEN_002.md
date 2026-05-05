---
code: E_LEN_002
title: Empty input array
severity: error
since: 0.5.7
---

# E_LEN_002 — Empty input array

A function received an empty array but needs at least one element.
Statistics, bootstrap sampling, and indicator computations can't
produce a value over zero observations.

## How to fix

Check the array length before calling, or filter out empty cases:

=== "Python"
    ```python
    if len(returns) > 0:
        ci = flox.bootstrap_ci(returns, confidence=0.95)
    else:
        ci = (float("nan"), float("nan"), float("nan"))
    ```

## Common causes

- An aggregator pipeline produced no bars (e.g. trades all on the same
  timestamp with sub-millisecond resolution lost to int64 conversion).
- `dropna()` removed every row.
- A symbol filter excluded all symbols.
