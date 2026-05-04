# RFC: Binding Architecture (Public Version)

The "why" behind the C ABI / pybind11 / NAPI / Codon split. Adapted from the internal RFC; this is the public summary. If you're trying to land a change that conflicts with one of these decisions, please open an issue first.

## Goal

A single source of truth for the FLOX public API surface so that:

- `flox_capi.h` is generated, not hand-edited.
- Codon bindings are generated.
- ABI snapshot diffing is mechanical.
- Stub artifacts (`.pyi`, `.d.ts`, `llms.txt`, MCP context) stay in sync with the shipped surface.

## Non-goals

- Replacing the C++ engine with an IDL-defined API. **C++ is the authoritative implementation.** IDL describes only the **export surface**.
- Auto-generating pybind11 / NAPI bindings (decision below — see "What is *not* generated").
- Migrating LiveEngine consumers (CCXT, custom transports) under the IDL. Connectors and transports stay binding-owned.
- Generating an RPC protocol. FLOX is in-process FFI, not network IPC. This rules out gRPC/protobuf-shaped IDLs.

## Constraints (from the surface audit)

The IDL must encode the conventions already present in the hand-written `flox_capi.h`:

1. **Opaque handles** with explicit lifetime (`Flox<X>Handle` typedef + paired `_create` / `_destroy`).
2. **Raw fixed-point convention** (`int64_t price_raw`, `int64_t qty_raw`, scaled by 1e8).
3. **Out-parameters** in two flavours: single value (`uint8_t flox_book_best_bid(handle, double* out)`) and composite struct (`void flox_get_symbol_context(handle, uint32_t sym, FloxSymbolContext* out)`).
4. **Pointer-out wrappers** (`*_p` variants) for runtimes that cannot consume structs by value (Codon / QuickJS).
5. **Parallel arrays** for L2 books (`bid_prices[], bid_qtys[], n_bids, ...`) and similar shapes.
6. **Sliced flat arrays** with header indices (book updates: `level_offset / bid_count / ask_count` slicing a single levels array).
7. **`\0`-separated string lists** (`flox_segment_merge_full(input_paths, num_paths, ...)`).
8. **Extension-hook callback bundles** — struct-of-fn-pointers + `user_data` (RiskManager, Executor, etc.) See [extension-hook-pattern.md](extension-hook-pattern.md).

## Options considered

### A — Hand-authored YAML / JSON IDL + custom codegen

Write a separate schema file describing every export. Codegen consumes the schema, emits `flox_capi.h` and bindings.

- **Pro:** language-neutral, expressive type system, no preprocessor gymnastics.
- **Con:** schema drifts from C++ engine signatures because they're disconnected. Adding a new C++ method means updating *two* sources. High risk of stale schema.

### B — libclang extraction from C++ headers

Parse the C++ engine headers directly with libclang, treat declarations annotated `[[clang::annotate("flox_export")]]` as the export set.

- **Pro:** zero schema duplication. Every export is defined exactly once, in C++.
- **Con:** libclang version drift between developers. Needs a stable annotation convention. Templates and explicit instantiations are ambiguous.

### C — Hybrid: annotated C++ headers + libclang extraction (chosen)

Same as B, but the annotations live on a dedicated **spec header** (`include/flox/capi/flox_capi_spec.hpp`) — not on the C++ engine declarations. The spec header is the IDL; the engine header is implementation.

- **Pro:** spec stays close to C, libclang parses it cheaply, conventions (handles, fixed-point, out-params) are encoded as macros (`FLOX_EXPORT(group = "...")`). No version drift between IDL and bindings — both come from the same parse.
- **Pro:** the spec is C, so it's grep-able, IDE-friendly, and bindings authors can read it directly.
- **Con:** still need to keep the spec aligned with the C++ engine surface. We accept this — it's the same friction as keeping a public API in sync with internal code.

**Decision: Option C.** The spec header is `include/flox/capi/flox_capi_spec.hpp`. Tooling lives in `tools/codegen/`.

## What *is* generated

Out of the spec header, the codegen pipeline emits:

- `include/flox/capi/flox_capi.h` (the live header bindings consume)
- `tools/codegen/golden/flox_capi.{h,codon,md}` (golden reference + Codon FFI + Markdown)
- `.api/c-api.snapshot` (signature snapshot for ABI gating)
- `mcp/flox_mcp/data/c-api.snapshot` (data the flox-mcp server bundles)

Run `bash tools/codegen/scripts/regenerate.sh` to rebuild all of those atomically. CI verifies they're in sync.

## What is *not* generated (and why)

**The pybind11 binding (`python/`) is hand-written.** It does **not** consume `flox_capi.h` directly. The pybind11 surface exposes Pythonic APIs (numpy arrays, context managers, kwargs, exception mapping) that intentionally don't mirror the C ABI 1:1. Auto-generating those from a C-API IR would mean either:

1. Producing a parallel **second** Python module that wraps the C ABI, parallel to the existing pybind11 one — confusing duplication for users.
2. Re-architecting the existing pybind11 binding to be C-API based — a massive refactor that would lose pybind11's idiomatic strengths (numpy buffer protocol for zero-copy, RAII, native exception hierarchy).

Same logic for the NAPI binding (`node/`) — it wraps C++ classes / C ABI handles directly to surface idiomatic Node features (TypedArrays, async, JS object shapes).

Neither path is worth the cost. **Performance specifically:** numpy zero-copy via the buffer protocol crosses C++↔Python without copies for bar arrays, indicator outputs, equity curves. A pure C-ABI binding via cffi/ctypes would cost 20–40% on hot paths — disqualifying for a high-frequency trading framework.

`.pyi` stubs continue to be **derived** from the built pybind11 module via `scripts/gen_pyi_stubs.py` (in CI as a sync gate). That covers the AI-DX angle without forcing auto-generation of binding code.

`.d.ts` is hand-written in `node/index.d.ts`. CI checks every NAPI export has a matching declaration via `scripts/check_dts_exports.py`. Same trade-off as `.pyi`.

If a future use case appears for "C ABI from Python via ctypes" (e.g. for a binding-free dependency), an `emit_pyi_capi.py` emitter could produce a thin ctypes wrapper distinct from the pybind11 module. This is a future possibility, not in-flight work.

## How the layers fit

The diagram in [architecture-overview.md](architecture-overview.md) shows the runtime layout. From an IDL-spec-edit perspective:

1. You edit `flox_capi_spec.hpp`.
2. `regenerate.sh` derives `flox_capi.h`, the Codon golden file, the markdown reference, and the snapshots.
3. You implement the C++ side in `src/capi/flox_capi.cpp`.
4. You **hand-write** the pybind11 wrapper in `python/` and the NAPI wrapper in `node/src/`.
5. The [parity gate](parity-gate.md) catches it if you forget steps 4.

## Coverage instead of generation

For pybind11 and NAPI, instead of generating, we use a manifest-based gate ([parity-gate.md](parity-gate.md)) that asserts every IDL group has the expected classes / functions in each binding. This is mechanical (catches "you forgot to wire pybind11") but doesn't constrain how the binding is written (you keep your numpy buffer protocol, your TypedArrays, your idiomatic exception types).

It's a coarser check than auto-generation, but it's the right trade for a framework where binding ergonomics matter as much as the C++ engine itself.

## Annotation convention

Everywhere in the spec header:

```cpp
#if defined(__clang__) && __has_cpp_attribute(clang::annotate)
#  define FLOX_EXPORT(...) [[clang::annotate("flox_export:" __VA_ARGS__)]]
#else
#  define FLOX_EXPORT(...)
#endif
```

Usage:

```cpp
FLOX_EXPORT(group = "metrics")
FloxPnLTrackerHandle flox_pnl_tracker_create(FloxPnLTrackerCallbacks cb);
```

Group strings are mandatory; they're how the parity gate knows which classes / functions belong together across bindings.

## Generation pipeline

```
                   spec header (flox_capi_spec.hpp)
                            │
                            ▼
                       libclang AST
                            │   (Python bindings: clang.cindex)
                            ▼
              ┌─── extractor.py ────┐
              │ - walk translation unit
              │ - collect FLOX_EXPORT decls
              │ - normalize types
              │ - resolve handles
              └────────┬────────────┘
                       ▼
                       IR (Python dataclass tree)
                       │
        ┌──────────────┼──────────────┬──────────────┬───────────────┐
        ▼              ▼              ▼              ▼               ▼
  emit_capi.py    emit_codon.py   emit_md.py    emit_snapshot.py   emit_mcp.py
        │              │              │              │               │
        ▼              ▼              ▼              ▼               ▼
   flox_capi.h    flox_capi.codon flox_capi.md  c-api.snapshot   mcp/data/
```

Each emitter is independent and operates on the same IR. Adding a new auto-generated binding is one new emitter file, no schema edits.

## Risks

1. **libclang version drift.** The codegen locks to a specific libclang release (the one CI uses for clang-format / clang-tidy). Contributors running a wildly different version locally may produce slightly different output. Mitigation: regenerate-and-diff in CI rather than trust the developer's local toolchain — that's `codegen-check`.

2. **Templates and explicit instantiation.** Annotating a template primary declaration is ambiguous. Convention: annotate the explicit instantiation that will be exported, or a thin non-template wrapper function.

3. **Schema↔engine drift.** Since the IDL is a separate header (not the engine itself), it's possible to add a C++ method without exporting it. This is intentional — most engine internals aren't user-facing. The opposite drift (export listed in IDL but engine method removed) surfaces as a link error in `src/capi/flox_capi.cpp` at compile time.

4. **Adding a binding-side hook without IDL.** Not possible: the [parity gate](parity-gate.md) requires every binding-exposed group to be declared in the IDL first.
