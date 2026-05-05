---
code: E_RUN_002
title: No data loaded
severity: error
since: 0.5.7
---

# E_RUN_002 — No data loaded

`Engine.run()` was called but no market data has been loaded. Load
OHLCV bars or trades before running.

## How to fix

=== "Python"
    ```python
    eng = flox.Engine()
    eng.add_symbol("BTCUSDT")
    eng.load_csv("BTCUSDT", "btc.csv")        # ← load first
    eng.run(MyStrategy())
    ```

For OHLCV-from-arrays:

```python
eng.load_ohlcv({
    "ts": ts_array,
    "open": open_array,
    "high": high_array,
    "low": low_array,
    "close": close_array,
    "volume": volume_array,
}, symbol="BTCUSDT")
```

## Common causes

- The data-loading call was skipped (e.g. only the symbol was registered).
- A typo in the symbol name caused `add_symbol("BTC")` and
  `load_csv("BTCUSDT", ...)` to refer to different symbols.
