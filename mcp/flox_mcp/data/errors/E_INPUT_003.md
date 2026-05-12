---
code: E_INPUT_003
title: Precision mismatch across tapes for same (exchange, name)
severity: error
since: 0.6.0
---

# E_INPUT_003 — Precision mismatch across tapes

`MergedTapeReader` keys symbols by `(metadata.exchange, name)`. Two
tapes claimed the same key but their manifests disagree on
`price_precision` or `qty_precision`. This is a data-quality issue:
the same instrument cannot have two different tick / step sizes.

## How to fix

Inspect the offending tapes' `metadata.json` files and reconcile:

```bash
jq '.symbols' tape-a/metadata.json
jq '.symbols' tape-b/metadata.json
```

If the precisions genuinely diverged (e.g. one capture predates an
exchange's precision change), either:

- exclude the older tape from the merge, or
- treat them as different venues by using a distinct
  `exchange_name` (e.g. `"bybit-legacy"` vs `"bybit"`) when recording,
  so the merge sees them as separate symbols.

## Common causes

- Mixing tapes from different capture stacks that set precisions
  inconsistently for the same symbol.
- Manual edits to `metadata.json`.
