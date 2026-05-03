---
code: E_LEN_001
title: Mismatched array lengths
severity: error
since: 0.1.0
---

# E_LEN_001 — Mismatched array lengths

A function that takes parallel arrays (e.g. `flox.permutation_test(x, y)`,
`flox.bootstrap_ci(...)`) was given inputs of different lengths.

## How to fix

Make the arrays the same length before calling.

```python
import numpy as np
import flox

x = np.array([1.0, 2.0, 3.0])
y = np.array([4.0, 5.0])           # ← different length, raises E_LEN_001

# Common fix: trim to min length, or align on a join key.
n = min(len(x), len(y))
flox.permutation_test(x[:n], y[:n])  # ✅
```

## Common causes

- Slicing two series with overlapping but non-equal date ranges.
- One input came from `Engine.close()` and the other from a vendor
  that has more / fewer bars (gaps, holidays, weekends).
- Forgetting to drop NaN tail/head from one of the arrays after
  applying a windowed indicator.

## Diagnosing

The exception message includes both sizes:

```python
try:
    flox.permutation_test(x, y)
except flox.FloxError as e:
    if e.code == "E_LEN_001":
        print(e.message)   # "Got x.size=3, y.size=2."
```

If the inputs come from different sources, align them first by
timestamp:

```python
import pandas as pd
df = pd.merge(
    pd.DataFrame({"ts": ts_x, "x": x}),
    pd.DataFrame({"ts": ts_y, "y": y}),
    on="ts",
    how="inner",
)
flox.permutation_test(df["x"].to_numpy(), df["y"].to_numpy())
```
