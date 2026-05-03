---
code: E_TIME_001
title: Unknown interval unit
severity: error
since: 0.1.0
---

# E_TIME_001 — Unknown interval unit

`Engine.resample(interval)` (and similar interval-accepting methods)
was given a string with an unrecognized unit suffix.

## Recognized formats

`"<n><unit>"` where `n` is an integer and `unit` is one of:

| Unit | Meaning |
|------|---------|
| `s`  | seconds |
| `m`  | minutes |
| `h`  | hours   |
| `d`  | days    |

Examples: `"30s"`, `"5m"`, `"1h"`, `"4h"`, `"1d"`.

## How to fix

Use one of the recognized unit suffixes.

```python
eng.resample("5m")    # 5 minutes  ✅
eng.resample("1h")    # 1 hour     ✅
eng.resample("1d")    # 1 day      ✅
eng.resample("5min")  # ❌ raises E_TIME_001 — use "5m"
eng.resample("4hr")   # ❌ raises E_TIME_001 — use "4h"
```

## Common causes

- Pandas-style suffixes (`min`, `hr`, `H`, `T`) — FLOX uses single-letter
  units only.
- ISO-8601 durations (`PT5M`) — not supported.
- Trailing whitespace or capital letters: `"1H"` is not the same as
  `"1h"`. Match is case-sensitive.

## Diagnosing

```python
try:
    eng.resample(user_input)
except flox.FloxError as e:
    if e.code == "E_TIME_001":
        print("Try one of: 1s, 30s, 1m, 5m, 1h, 4h, 1d")
```
