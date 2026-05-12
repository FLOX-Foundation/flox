---
code: E_INPUT_002
title: Cannot load tape metadata.json
severity: error
since: 0.6.0
---

# E_INPUT_002 — Cannot load tape metadata.json

A consumer that needs `metadata.json` (e.g. `MergedTapeReader`) tried
to open it from a `.floxlog` directory and got nothing back. Either
the file is missing, unreadable, or the path itself is wrong.

## How to fix

Verify the directory layout. Every `.floxlog` directory written by
flox carries a `metadata.json` next to its segment files:

```
mytape/
├── metadata.json
├── 1778572198427000000.floxlog
└── ...
```

If `metadata.json` is missing the directory was either
- written by a writer that crashed before `stop()` (no metadata flush
  point), or
- copied / synced incompletely.

For `MergedTapeReader` specifically the path must be the **tape
directory**, not one of its segment files.

## Common causes

- Pointing at a parent directory or a segment file.
- Tapes written by very old / partial builds that did not emit
  metadata. Re-record or hand-write a minimal `metadata.json`.
