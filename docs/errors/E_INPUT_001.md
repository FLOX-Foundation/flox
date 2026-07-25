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

The name is a constructor argument on the two Python types that own a
binary-log writer — `DataWriter` (offline writing) and
`BinaryLogRecorderHook` (recording a live engine). Pass one of the two
accepted spellings:

=== "Python"
    ```python
    import flox_py as flox

    writer = flox.DataWriter("out", compression="lz4")        # or "none"

    rec = flox.BinaryLogRecorderHook(
        "out",
        compression="lz4",
        exchange_name="bybit",
        instrument_type="perpetual",
    )
    ```

Node.js never raises this code. `new flox.DataWriter(dir, maxSegmentMb,
exchangeId)` takes no compression argument and always writes
uncompressed; `new flox.BinaryLogRecorderHook(dir, maxSegmentMb,
exchangeId, compression)` does take one, but rejects a bad value with a
plain `TypeError` (`compression must be "none" or "lz4"`).

## Common causes

- Typo: `"lz4hc"` / `"zstd"` / `"gzip"` — only `lz4` is currently
  built in. Adding more compressors is a separate task.
- `"lz4"` is accepted by the argument check regardless of how the engine
  was built. If it was configured with `-DFLOX_ENABLE_LZ4=OFF` (the
  default is ON, with a vendored fallback), the compressor is compiled
  out and silently produces zero bytes instead of raising here.
- The same code is also raised by `MergedTapeReader` for an empty tape
  list, which is an unrelated input error with its own message
  (`"paths list is empty"`).
