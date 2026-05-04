# Cross-Binding Parity Gate

How `scripts/check_binding_parity.py` makes "I added a function to the C ABI but forgot the pybind11/NAPI wrapper" a CI failure instead of a silent gap that surfaces months later when a user complains.

## The problem it solves

The C ABI surface (`flox_capi.h`) grows whenever someone adds a `FLOX_EXPORT` to the IDL. Each binding (pybind11, NAPI, Codon) is supposed to expose that addition. Codon is auto-generated, so it's automatic. **pybind11 and NAPI are hand-written**, so they drift.

Before this gate existed, the only way to notice a gap was for a Python or Node user to say "where's the `Executor` class?" — usually months after the C ABI shipped it. The gate catches it on the PR that introduces the drift.

## How it works

The script:

1. **Parses the IDL** ([`include/flox/capi/flox_capi_spec.hpp`](../../include/flox/capi/flox_capi_spec.hpp)). Every `FLOX_EXPORT(group = "X")` declaration belongs to a named group.
2. **Reads the manifest** ([`tools/codegen/binding_parity.yaml`](../../tools/codegen/binding_parity.yaml)). Each group has up to four entries: `pybind11`, `napi`, `codon`, `quickjs`.
3. **Scans the bindings** for the symbols each entry promises.
   - pybind11: parses `python/flox_py/_flox_py/__init__.pyi` for `class X:` and `def x(...):`
   - NAPI: parses `node/index.d.ts` for `export class X` / `export interface X` / `export function x`
   - Codon: checks the auto-generated `tools/codegen/golden/flox_capi.codon` for the group section
4. **Fails loudly** if anything is missing.

Run locally:

```bash
python3 scripts/check_binding_parity.py            # exit 1 on drift
python3 scripts/check_binding_parity.py --verbose  # show every group, not just failures
```

Wired into CI as the "Verify cross-binding parity" step inside the `verify-docs-current` job.

## Status values

Each per-binding entry has one of three statuses:

### `required`

The binding **must** expose the listed symbols. List them under `classes` or `functions`:

```yaml
metrics:
  pybind11: { status: required, classes: [PnLTracker] }
  napi: { status: required, classes: [PnLTracker] }
  codon: { status: required }
```

This is the default for every user-facing surface. If the listed class or function disappears, CI fails.

### `not_applicable`

This group is internal helpers and **never** intended for this binding. No symbol list:

```yaml
fixed_point:
  pybind11: { status: not_applicable }   # exposed via Price.toDouble() on classes
  napi: { status: not_applicable }
  codon: { status: required }
```

Used for things like `pointer_out` (output-pointer helpers) or `validation` (input validation primitives) that wouldn't make sense as a Python class.

### `allowlist`

A **known gap**, with a `reason` string. The reason is mandatory — CI fails if it's empty or missing:

```yaml
your_group:
  napi:
    status: allowlist
    reason: "stats functions exposed via Stats class methods; function-style not declared. Cosmetic gap; tracked under <issue>."
```

Use sparingly. The point of the gate is to surface gaps; allowlisting them just kicks the can. If a gap is intentional and stable, it should be `not_applicable` instead. If it's "we'll do this next sprint", that's `allowlist` — but link the tracker so it doesn't get forgotten.

## Adding a new IDL group

If you add `FLOX_EXPORT(group = "new_thing")` and don't update the YAML, CI says:

```
FAIL  new_thing  config  missing_yaml: add an entry to binding_parity.yaml
```

Add a stanza for the new group:

```yaml
new_thing:
  pybind11: { status: required, classes: [NewThing] }
  napi: { status: required, classes: [NewThing] }
  codon: { status: required }
```

Then make sure the listed classes / functions actually exist on each side.

## Removing a group

If you delete the last `FLOX_EXPORT(group = "old_thing")` declaration, the gate notices the YAML still mentions it:

```
FAIL  old_thing  config  missing_in_binding: group `old_thing` not found in IDL spec; remove from yaml
```

Remove the YAML stanza in the same PR.

## Why this is just a manifest, not auto-generation

Reasonable question: "if you know which classes belong to which group, why not generate the bindings from that?"

Two reasons:

1. **The manifest knows the *names*, not the *shape*.** It can say "`Executor` should exist", but it can't generate the `submit / cancel / replace / submit_oco / capabilities` method signatures — those are codegen's job (and codegen is unsafe to run for pybind11/NAPI for [perf reasons](architecture-overview.md#why-this-layout-not-c-abi-all-the-way)).
2. **Coverage ≠ correctness.** Even if `Executor` is named in `.pyi`, that doesn't mean the binding actually wires `flox_executor_*` correctly. Tests check correctness; the gate checks "did you remember to write the wrapper at all".

So the gate is a coarse mechanical check: "does the symbol exist?". Tests cover the semantics. Together they catch most of what auto-generation would catch, without giving up idiomatic bindings.

## Scope of the gate (what it doesn't catch)

- Argument signature drift (pybind11 method takes `int` but C ABI takes `int64_t`). That's pybind11's responsibility at runtime.
- Submodule-style exposure (`flox.targets.linear_slope` rather than top-level `flox.LinearSlope`). The gate only inspects top-level declarations. The `targets` group is `allowlist`'ed for this reason.
- Behavioral parity (Python and Node returning different values for the same input). Cross-binding parity *tests* (`scripts/cross_binding_parity.py`) cover that.

## Extending the gate

The script is small (~280 lines, [scripts/check_binding_parity.py](../../scripts/check_binding_parity.py)). If you need a new check, add it there. Examples:

- **QuickJS coverage.** Currently QuickJS is unchecked because its bindings live in `.cpp` and aren't easily introspectable. Could be added by parsing `JS_NewCFunction` calls in `src/quickjs/js_bindings.cpp`.
- **Method-level checks.** Currently we check class presence; we could check that a class has specific methods (e.g. `Executor` must have `submit`, `cancel`, `replace`).

Both would tighten the gate. Keep changes minimal — every false-positive case wastes contributor time.
