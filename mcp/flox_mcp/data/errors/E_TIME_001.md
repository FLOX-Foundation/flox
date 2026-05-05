---
code: E_TIME_001
title: Unknown interval unit
severity: error
since: 0.5.7
---

# E_TIME_001 — Unknown interval unit

An interval string like `"5x"` was passed where the unit suffix is not
recognised. FLOX accepts:

| Unit | Meaning |
|------|---------|
| `s`  | seconds |
| `m`  | minutes |
| `h`  | hours   |
| `d`  | days    |

## How to fix

=== "Python"
    ```python
    eng.resample("5m")          # 5 minutes — OK
    eng.resample("1h")          # 1 hour — OK
    eng.resample("15s")         # 15 seconds — OK
    eng.resample("1d")          # 1 day — OK
    # Not OK: "5min", "5 min", "1H", "five-minute"
    ```

## Common causes

- Pandas-style suffixes (`"5min"`, `"1H"`) — FLOX uses single-character
  units to avoid the `M` (month) / `m` (minute) ambiguity.
- Whitespace in the string.
- Empty unit (`"5"`) — always provide a unit.
