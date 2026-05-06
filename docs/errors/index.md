# Error code reference

Each FloxError carries a stable code (e.g. `E_SYM_001`), a human message,
and a help URL pointing to the per-code page in this section. AI agents
reading FLOX errors can resolve the code to a documented fix without
parsing free-text messages.

## Code format

`E_<DOMAIN>_<NUMBER>` — `DOMAIN` is a short uppercase tag identifying the
subsystem; `NUMBER` is a zero-padded 3-digit counter within that domain.
Codes never change meaning once published; if the cause shifts, a new
code is allocated.

| Domain    | Subsystem                                        |
|-----------|--------------------------------------------------|
| `SYM`     | Symbol registry / lookups                         |
| `DATA`    | Input data loading and parsing                    |
| `KEY`     | Event field lookups and required keys             |
| `TIME`    | Calendar, intervals, timestamps                   |
| `LEN`     | Array length mismatches                           |
| `ORDER`   | Order submission and lifecycle                    |
| `RISK`    | Pre-trade risk hooks                              |
| `IDX`     | Index out of range                                |
| `IO`      | Filesystem and binary log I/O                     |
| `CONFIG`  | Configuration validation                          |

## Catalog

| Code           | Message                                                       |
|----------------|---------------------------------------------------------------|
| [`E_SYM_001`](E_SYM_001.md) | Symbol is not registered                       |
| [`E_RUN_003`](E_RUN_003.md) | Invalid factory return value (GridSearch)      |

## How to add a new code

1. Decide on a domain. If a new domain is needed, add it to the table
   above.
2. Pick the next number in that domain.
3. Write a Markdown page `docs/errors/<code>.md` with the frontmatter
   shown below.
4. Use the code at the throw site:

    ```cpp
    throw flox::FloxError(
        "E_SYM_001",
        "Symbol '" + name + "' is not registered. ...");
    ```

5. Run `python3 scripts/check_error_codes.py` (it also runs in CI). It
   verifies every code referenced from source has a Markdown page and
   vice-versa.

### Frontmatter convention

```yaml
---
code: E_SYM_001
title: Symbol is not registered
severity: error
since: 0.1.0
---
```
