---
code: E_INPUT_001
title: Unknown compression type
severity: error
since: 0.5.7
---

# E_INPUT_001 — Unknown compression type

A compression name passed to a recorder / writer is not one FLOX
supports. Accepted values:

| Value          | Effect |
|----------------|--------|
| `"none"` / `""` | No compression (default) |
| `"lz4"`        | LZ4 frame compression |

## How to fix

=== "Python"
    ```python
    cfg = flox.RecordingConfig(
        output_dir="out",
        compression="lz4",        # or "none"
    )
    ```

## Common causes

- Typo: `"lz4hc"` / `"zstd"` / `"gzip"` — only `lz4` is currently
  built in. Adding more compressors is a separate task.
