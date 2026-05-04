# Adding a C ABI Export

Step-by-step for adding a new function to the FLOX public C ABI surface (anything in `flox_capi.h`).

If you're adding an extension hook (PnLTracker, Executor, etc.), follow [extension-hook-pattern.md](extension-hook-pattern.md) instead — that's a specialized variant of this flow.

## Mental model

```
include/flox/capi/flox_capi_spec.hpp     ← you edit this (the IDL)
        │
        │  bash tools/codegen/scripts/regenerate.sh
        ▼
include/flox/capi/flox_capi.h            ← regenerated
.api/c-api.snapshot                       ← regenerated (ABI signature gate)
mcp/flox_mcp/data/c-api.snapshot          ← regenerated (MCP context)
tools/codegen/golden/flox_capi.{h,codon,md}  ← regenerated
        │
        │  you implement in src/capi/flox_capi.cpp
        ▼
libflox_capi.so                           ← compiled

then bindings:
  python/    pybind11 wrapper       (hand-written, calls flox_xxx_*)
  node/      NAPI wrapper           (hand-written)
  codon/     auto-imported via golden
  quickjs/   hand-written wrapper

then docs sync (eight scripts, see ci-pipeline.md):
  scripts/gen_pyi_stubs.py     ← regenerates Python .pyi
  scripts/gen_api_index.py     ← regenerates docs/reference/python/_api_index.md
  scripts/gen_llms_txt.py      ← regenerates docs/llms*.txt
  scripts/sync_mcp_data.py     ← regenerates mcp/flox_mcp/data/
```

## Step 1: edit the IDL

In [`include/flox/capi/flox_capi_spec.hpp`](../../include/flox/capi/flox_capi_spec.hpp):

```cpp
FLOX_EXPORT(group = "your_group")
void flox_your_function(FloxRunnerHandle runner,
                        uint32_t symbol,
                        double param);
```

The `group = "..."` string is required — every export belongs to a group, and the [parity gate](parity-gate.md) checks per-group coverage in pybind11 / NAPI / codon.

If you're adding a new group, also add a stanza in [`tools/codegen/binding_parity.yaml`](../../tools/codegen/binding_parity.yaml). CI fails until you do.

## Step 2: regenerate codegen artifacts

```bash
bash tools/codegen/scripts/regenerate.sh
```

This single command updates:
- `include/flox/capi/flox_capi.h` (the live header)
- `tools/codegen/golden/flox_capi.{h,codon,md}` (the golden reference)
- `.api/c-api.snapshot` (signature snapshot for ABI gating)
- `mcp/flox_mcp/data/*` (data the flox-mcp server bundles)

Then verify nothing drifted:

```bash
bash tools/codegen/scripts/check.sh
```

## Step 3: implement in C++

In [`src/capi/flox_capi.cpp`](../../src/capi/flox_capi.cpp):

```cpp
void flox_your_function(FloxRunnerHandle h, uint32_t symbol, double param) {
  toRunner(h)->yourFunction(symbol, param);
}
```

If the function calls into C++ engine code that doesn't yet exist, write that first. The C ABI wrapper is a thin translation layer; business logic lives in `src/` proper.

## Step 4: build and test the C side

```bash
cmake --build build
ctest --test-dir build
```

Add a GTest case under `tests/` if the function has non-trivial semantics (anything beyond a one-line delegation). Pattern: `tests/test_capi_*.cpp` — link against `flox_capi`, exercise the function via the C ABI directly.

## Step 5: pybind11 wrapper

Open [`python/strategy_bindings.h`](../../python/strategy_bindings.h) (or the appropriate `python/*_bindings.h` file matching your group). Add a method on the relevant `Py*` class that calls your C ABI function. Then register it in the `py::class_<...>` block.

Example:

```cpp
class PyStrategyRunner {
  // ...
  void your_function(uint32_t symbol, double param) {
    flox_your_function(_runner, symbol, param);
  }
};
```

```cpp
py::class_<PyStrategyRunner>(m, "Runner")
    // ...
    .def("your_function", &PyStrategyRunner::your_function,
         py::arg("symbol"), py::arg("param"));
```

Build the Python module:

```bash
cmake --build build --target _flox_py
```

Regenerate the `.pyi` stubs (running pybind11 module is the source of truth):

```bash
PYTHONPATH=build/python python3 scripts/gen_pyi_stubs.py
```

## Step 6: NAPI wrapper

Open [`node/src/strategy.h`](../../node/src/strategy.h) (or the appropriate `node/src/*.h` file). Add an `InstanceMethod` entry to the class's `Init` and a corresponding method that calls the C ABI:

```cpp
Napi::Value yourFunction(const Napi::CallbackInfo& info) {
  uint32_t sym = symId(info[0]);
  double param = info[1].As<Napi::Number>().DoubleValue();
  flox_your_function(_handle, sym, param);
  return info.Env().Undefined();
}
```

Add the TypeScript declaration in [`node/index.d.ts`](../../node/index.d.ts):

```typescript
yourFunction(symbol: Symbol | number, param: number): void;
```

Rebuild and verify:

```bash
cd node && npm run build && npm run typecheck
node test/test_bindings.js
```

## Step 7: parity gate

If your group is new (not yet in `binding_parity.yaml`), add it. If your group exists, your function should be covered already — check by running:

```bash
python3 scripts/check_binding_parity.py
```

The gate scans the IDL for `FLOX_EXPORT(group = "X")` declarations and verifies every group has the declared classes / functions in pybind11 (`.pyi`) and NAPI (`.d.ts`).

For function-shaped groups (no class wrapping), list expected function names:

```yaml
your_group:
  pybind11: { status: required, functions: [your_function] }
  napi: { status: required, functions: [yourFunction] }
  codon: { status: required }
```

## Step 8: docs sync chain

Three scripts re-derive documentation from the bindings — run them in order:

```bash
python3 scripts/gen_pyi_stubs.py        # 1. .pyi from running pybind11
python3 scripts/gen_api_index.py        # 2. docs/reference/python/_api_index.md from .pyi
python3 scripts/gen_llms_txt.py         # 3. docs/llms*.txt (embeds api_index)
```

`sync_mcp_data.py` runs as part of `regenerate.sh`, so it's already up to date.

Verify with `--check` flags before committing:

```bash
python3 scripts/gen_pyi_stubs.py --check
python3 scripts/gen_api_index.py --check
python3 scripts/gen_llms_txt.py --check
```

If any drifts, the generated form differs from what's in the repo — re-run the generator without `--check` and commit the diff.

## Step 9: commit

Stage the regenerated artifacts plus your new code:

```bash
git add include/flox/capi/flox_capi.h \
        include/flox/capi/flox_capi_spec.hpp \
        src/capi/flox_capi.cpp \
        .api/c-api.snapshot \
        mcp/flox_mcp/data/c-api.snapshot \
        tools/codegen/golden/ \
        python/flox_py/_flox_py/__init__.pyi \
        docs/reference/python/_api_index.md \
        docs/llms*.txt \
        python/<your binding header> \
        node/src/<your binding header> \
        node/index.d.ts \
        tools/codegen/binding_parity.yaml \
        tests/<your test>.cpp
```

Don't `git add -A` — `analysis/` and `build*/` are gitignored but other untracked artifacts (screenshots, scratch notes) might be present and shouldn't go in the commit.

## CI

If everything above passes locally, CI will pass too — the same scripts run there. The fast-fail pipeline ([ci-pipeline.md](ci-pipeline.md)) runs `format-check` and `verify-docs-current` before the OS build matrix, so if you forgot to regenerate something the failure surfaces in ~30 seconds.
