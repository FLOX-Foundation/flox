# Binding Architecture Overview

How FLOX exposes a single C++ engine to Python, Node.js, Codon, and QuickJS — and why each path is shaped the way it is.

## Layers

```
┌─────────────────────────────────────────────────────────────────────┐
│  C++ engine (src/, include/flox/) — the authoritative implementation │
└─────────────────────────────────────────────────────────────────────┘
                              ▲
                              │ thin C ABI wrapper (src/capi/flox_capi.cpp)
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│  C ABI surface (include/flox/capi/flox_capi.h)                       │
│  • declared in IDL spec (flox_capi_spec.hpp)                         │
│  • frozen via .api/c-api.snapshot                                    │
│  • 328+ functions, opaque handles, raw fixed-point integers          │
└─────────────────────────────────────────────────────────────────────┘
              │             │             │             │
              ▼             ▼             ▼             ▼
        ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐
        │ pybind11│   │  NAPI   │   │  Codon  │   │ QuickJS │
        │ python/ │   │  node/  │   │ codon/  │   │ quickjs/│
        └─────────┘   └─────────┘   └─────────┘   └─────────┘
        idiomatic     idiomatic     auto-gen      auto-gen
        Python        Node.js       Codon FFI     JS bindings
```

## Why this layout (not "C ABI all the way")

A reasonable question: "why not generate every binding from the C ABI? Single source of truth, bindings come for free."

Two reasons it isn't done:

**Performance.** Python's [pybind11](https://pybind11.readthedocs.io/) gives numpy zero-copy via the [buffer protocol](https://docs.python.org/3/c-api/buffer.html) — bar arrays, indicator outputs, position equity curves cross C++↔Python without copying. A pure C ABI binding via cffi/ctypes requires either a copy or hand-rolled `ctypes.c_double * N` machinery, costing 20–40% on hot paths (large backtests, tick data ingestion). FLOX is a high-frequency trading framework; we don't trade that.

Same logic applies to NAPI: native async / `Promise` / `Worker` patterns are ergonomic in Node and slow to fake from `node-ffi` / `koffi`.

**Idiom.** Python users expect context managers, native exception hierarchies, type hints in their editor. Node users expect TypedArrays, `class extends`, Promise-based APIs. Auto-generating those from a C-shaped IR produces "C with a Python accent" — works, but every line of user code has to bend around the C ABI's choices instead of the language's.

**Decision:** every binding is shaped to its language. The C ABI exists so Codon / QuickJS / future-Rust / future-Go can be wired up with codegen, while pybind11 / NAPI stay hand-written with idiom freedom.

## Per-binding paths

### pybind11 (`python/`)

- Wraps **C ABI handles**, not C++ classes directly. `python/strategy_bindings.h` calls `flox_runner_create`, `flox_runner_add_strategy`, etc. — same surface Codon and QuickJS see.
- One exception: `PyBacktestRunner` wraps `flox::BacktestRunner` (C++) directly. It predates the C ABI on that surface; the hook setters work around it via `cxx_adapters` in `python/hook_bindings.h` that implement `flox::IOrderExecutor` / `IOrderExecutionListener` rather than going through C ABI handles. Subject to migration once the C ABI replay-source path covers OHLCV / CSV.
- `.pyi` stubs auto-generated from the live module by `scripts/gen_pyi_stubs.py`. Type checkers and AI agents see the full typed surface.

### NAPI (`node/`)

- All wrappers go through C ABI handles (`FloxRunnerHandle`, `FloxBacktestRunnerHandle`, etc.) — `node/src/strategy.h`, `backtest.h`, `engine.h`.
- TypeScript declarations hand-written in `node/index.d.ts`. CI verifies declared exports match the C++ binding (`scripts/check_dts_exports.py`) and that `tsc --noEmit` passes (`node/test/test_types.ts`).

### Codon (`codon/`, `tools/codegen/golden/flox_capi.codon`)

- 100% generated from the IDL spec by `tools/codegen/flox_codegen/emit_codon.py`. Each `FLOX_EXPORT(...)` becomes a `from C import ...` declaration.
- Strategy / backtest examples in `codon/flox/` consume the golden file via re-export.

### QuickJS (`src/quickjs/`)

- Hand-written bindings in `js_bindings.cpp` / `js_strategy.cpp`, all going through C ABI. Used for the embedded JS runner; not the same module as NAPI.

## Build / packaging

| Binding   | Build mechanism                         | Distributed as                    |
|-----------|-----------------------------------------|-----------------------------------|
| pybind11  | CMake target `_flox_py` via pybind11    | Wheel on PyPI (`flox-py`)         |
| NAPI      | `cmake-js` via `node/CMakeLists.txt`    | npm package (`@flox-foundation/flox`) |
| Codon    | Codon compiler emits native binaries     | Linked against `libflox_capi.so`  |
| QuickJS   | CMake target `flox_js_runner`           | In-tree binary                    |

The C++ engine and C ABI compile into `libflox_capi.so` (Linux) / `.dylib` (macOS) / `.dll` (Windows). All binding modules link against it.

## Where to look next

- **Adding a new C ABI export** end-to-end: see [adding-a-c-api-export.md](adding-a-c-api-export.md).
- **Adding an extension hook** (PnLTracker / Executor / etc.): see [extension-hook-pattern.md](extension-hook-pattern.md).
- **Cross-binding coverage gate** (catches "I forgot to wire pybind11 / NAPI"): see [parity-gate.md](parity-gate.md).
- **CI pipeline** (fast-fail, docs sync chain): see [ci-pipeline.md](ci-pipeline.md).
