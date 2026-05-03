---
code: E_DATA_001
title: Engine has no data loaded
severity: error
since: 0.1.0
---

# E_DATA_001 — Engine has no data loaded

A method that needs OHLCV bars (`Engine.bar_count()`, `Engine.close()`,
`Engine.run()`, etc.) was called before any data-loading method
(`load_csv` / `load_ohlcv` / `load_df`) succeeded.

## How to fix

Load data **before** querying it.

```python
import flox

eng = flox.Engine()
eng.load_csv("btcusdt_1m.csv")        # ← required first
print(eng.bar_count(), eng.close()[-1])
```

## Common causes

- Calling `eng.run(sb)` immediately after constructing the Engine.
  Engines start empty; load first.
- A previous `load_*` call **raised** (e.g. file not found,
  malformed CSV) and the loaded state stayed empty.
  Inspect the exception — don't swallow it.
- Re-using an Engine across symbols and forgetting to load each.

## Diagnosing

```python
try:
    eng.bar_count()
except flox.FloxError as e:
    if e.code == "E_DATA_001":
        print(f"loaded symbols: {sorted(eng.symbols())}")
        print(f"hint: call eng.load_csv(...) or eng.load_ohlcv(...)")
```
