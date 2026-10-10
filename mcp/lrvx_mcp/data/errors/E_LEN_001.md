---
code: E_LEN_001
title: Array length mismatch
severity: error
since: 0.5.7
---

# E_LEN_001 — Array length mismatch

Two or more input arrays have different lengths but were expected to be
parallel (i.e. each index represents the same time / row across all
arrays). Common offenders: OHLCV bars where `high` / `low` / `close` /
`volume` don't match `open`, or trade arrays where `prices` /
`quantities` / `is_buy` don't match.

## How to fix

Pad / trim the arrays so they all share the same length, then re-pass
to the API:

=== "Python"
    ```python
    import numpy as np

    # Make sure every array has the same length before passing.
    n = min(len(opens), len(highs), len(lows), len(closes))
    eng.load_ohlcv({
        "ts": ts[:n],
        "open": opens[:n],
        "high": highs[:n],
        "low": lows[:n],
        "close": closes[:n],
        "volume": volumes[:n],
    }, symbol="BTCUSDT")
    ```

=== "Node.js"
    ```js
    const n = Math.min(opens.length, highs.length, lows.length, closes.length);
    runner.runOhlcv(ts.slice(0, n), closes.slice(0, n), "BTC");
    ```

## Common causes

- Forgetting that pandas `dropna()` operates per-column — re-run on the
  full DataFrame, not on each Series individually.
- Indicator computations producing shorter outputs than inputs (rolling
  windows) — align with the input length before passing.
- Concatenating data from sources that disagree on the calendar.
