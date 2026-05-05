---
code: E_KEY_001
title: Required key missing
severity: error
since: 0.5.7
---

# E_KEY_001 — Required key missing

A dict / object passed to a FLOX API was missing a required key. The
error message names the missing key.

## Where it fires

- `Engine.load_ohlcv(d)` — expects `ts` (or `timestamp`), `open`,
  `high`, `low`, `close`, `volume`.

## How to fix

=== "Python"
    ```python
    eng.load_ohlcv({
        "ts":     ts_array,        # int64 ns timestamps
        "open":   opens,
        "high":   highs,
        "low":    lows,
        "close":  closes,
        "volume": volumes,
    }, symbol="BTCUSDT")
    ```

If your DataFrame uses different column names, rename them:

```python
df = df.rename(columns={"timestamp": "ts", "vol": "volume"})
eng.load_ohlcv({k: df[k].to_numpy() for k in
                ["ts", "open", "high", "low", "close", "volume"]},
               symbol="BTCUSDT")
```

## Common causes

- Source data uses `timestamp_ns` / `time` / `t` instead of `ts` —
  rename before passing.
- Volume series stored as `vol` — same fix.
- Trying to load a tick stream into the OHLCV path.
