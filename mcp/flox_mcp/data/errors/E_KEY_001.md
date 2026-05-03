---
code: E_KEY_001
title: Missing required column in OHLCV input
severity: error
since: 0.1.0
---

# E_KEY_001 — Missing required column in OHLCV input

A method that accepts a dict of NumPy arrays (`Engine.load_ohlcv(dict)`,
similar paths) was given a dict missing one of the required columns.

## Required keys

- `open`, `high`, `low`, `close`, `volume` — `float64` arrays.
- `ts` **or** `timestamp` — `int64` array of nanoseconds (or seconds —
  auto-normalized; smaller magnitudes are scaled up).

All arrays must be the same length.

## How to fix

Make sure the dict has every required column.

```python
import numpy as np
import flox

eng = flox.Engine()
eng.load_ohlcv({
    "ts":     np.array([1700000000_000_000_000, ...], dtype=np.int64),
    "open":   np.array([60000.0, ...], dtype=np.float64),
    "high":   np.array([60100.0, ...], dtype=np.float64),
    "low":    np.array([59900.0, ...], dtype=np.float64),
    "close":  np.array([60050.0, ...], dtype=np.float64),
    "volume": np.array([12.5, ...],   dtype=np.float64),
})
```

## Common causes

- Pulling from a DataFrame and dropping the timestamp column on accident:
  ```python
  d = df[["open", "high", "low", "close", "volume"]].to_dict("series")
  eng.load_ohlcv(d)   # ← missing ts; raises E_KEY_001
  ```
  Fix: include `ts` in the column list.
- Using `time` or `t` as the timestamp column name. Only `ts` and
  `timestamp` are recognized.
- Passing a single OHLC bar instead of arrays — the API expects
  vectorized input.

## Diagnosing

The exception message tells you which key is missing:

```python
try:
    eng.load_ohlcv(d)
except flox.FloxError as e:
    if e.code == "E_KEY_001":
        print(f"got: {sorted(d.keys())}")
        print(f"missing: {e.message}")
```
