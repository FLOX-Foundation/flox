# Contributor Guide

Internal architecture and workflow documentation for people *changing* FLOX (not consuming it). User-facing API docs live elsewhere — start at the top of [docs/](../index.md).

## Start here

| If you want to... | Read |
|---|---|
| Understand how the bindings fit together | [architecture-overview.md](architecture-overview.md) |
| Add a function to `flox_capi.h` | [adding-a-c-api-export.md](adding-a-c-api-export.md) |
| Add a callback hook (PnLTracker / Executor / etc.) | [extension-hook-pattern.md](extension-hook-pattern.md) |
| Understand the cross-binding coverage gate | [parity-gate.md](parity-gate.md) |
| Debug CI / understand which scripts regenerate what | [ci-pipeline.md](ci-pipeline.md) |
| Know *why* the architecture is what it is | [binding-architecture-rfc.md](binding-architecture-rfc.md) |

## Mental model in one paragraph

FLOX is a C++ engine wrapped by a stable C ABI. The C ABI is generated from an IDL spec (`flox_capi_spec.hpp`). Codon and QuickJS bindings are auto-generated from the same spec. Python (pybind11) and Node (NAPI) bindings are **hand-written** so they can use language-idiomatic features (numpy zero-copy, async/await, native exception types) that auto-generation would lose. A coverage gate (`scripts/check_binding_parity.py`) catches "I added a C ABI export but forgot the pybind11/NAPI wrapper" at PR review time. CI is structured fast-fail: cheap checks (format, docs sync, parity) run first; the multi-OS build matrix only starts if those pass.

## When to update these docs

- **Adding a new binding** (Rust, Go, Swift): update `architecture-overview.md` to describe the new layer; add per-binding rules to `parity-gate.md`.
- **Changing the codegen pipeline**: update `binding-architecture-rfc.md` with the new emitter / IR change.
- **Adding a doc-sync script**: add it to the docs sync chain in `ci-pipeline.md` and `adding-a-c-api-export.md`.
- **Adding a new gate to CI**: document it under `ci-pipeline.md`'s "What runs in each gate" table.

This is a small set of docs by design — we don't need formal ADRs for every implementation choice. If a decision is non-obvious enough to warrant explanation, it goes inline in the relevant guide above.
