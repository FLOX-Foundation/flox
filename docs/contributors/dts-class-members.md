# Keeping node/index.d.ts class bodies in sync

`scripts/check_dts_exports.py` verifies that every NAPI top-level
export (class name, function name) appears in `node/index.d.ts`.
That check does not look inside class bodies, so a new method on
an existing wrap class slips through silently — the build passes,
TypeScript users get no autocomplete, the missing method becomes
`any` at usage sites.

T018, T021, T022, and T026 each introduced new NAPI methods that
shipped without matching d.ts entries. `scripts/check_dts_class_members.py`
runs in CI alongside the other verify-docs-current gates and fails
the PR before the gap reaches main.

## The rule

For every wrap class `XxxWrap` in `node/src/*.h`:

- Strip the `Wrap` suffix to get the TS export name (or look up
  `DTS_ALIASES` for the rare mismatch case).
- Every `InstanceMethod("name", ...)` in the wrap's `DefineClass`
  block must appear as a method in the matching `export class Xxx`
  body of `node/index.d.ts`.
- Extra d.ts methods are allowed (hand-written aliases, overloads).
  Missing d.ts methods fail.

## What to do when the gate fails

The error tells you which class is missing which method names. Add
the corresponding TypeScript signatures to `node/index.d.ts`.
Example fix from T018:

```ts
export class OrderGroup {
  // …existing methods…
  recordReplaceAccepted(legIndex: number, newOrderId: number): void;
  recordReplaceRejected(legIndex: number): void;
  findLegByOrderId(orderId: number): number | null;
}
```

The signatures should reflect the arguments NAPI actually unwraps.
Read the wrap method body in `node/src/<file>.h` for the cast types.

## Running locally

```
python3 scripts/check_dts_class_members.py
```

The script prints a coverage map of every wrap class with its
method count and any missing-from-d.ts count. A clean run ends with
`OK — every NAPI InstanceMethod is declared in node/index.d.ts.`

## Scope

- One-direction check (NAPI → d.ts). The reverse direction (extra
  d.ts methods) is intentionally permissive because some wrap
  helpers are not exposed at the InstanceMethod surface (e.g.
  static factories registered separately).
- Class name alias map `DTS_ALIASES` at the top of the script
  handles the rare case where the d.ts export name differs from
  the wrap class name minus `Wrap`. Keep it short.
- The script does not parse TypeScript signatures — it only
  verifies presence by name.
