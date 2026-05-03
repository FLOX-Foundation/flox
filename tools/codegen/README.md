# tools/codegen — IDL-driven code generation for the FLOX C API

Generates `flox_capi.h` and (in a follow-up PR) per-binding stub artifacts
(`.pyi`, `.d.ts`, Codon, llms.txt) from a single annotated source of truth
in `include/flox/capi/flox_capi_spec.hpp`.

Status: **full coverage**. Spec covers all 290 functions, 22 handles,
25 structs, 7 callback typedefs, and 2 enums in the live `flox_capi.h`.
The CI gate enforces full signature equivalence (`--require-full-coverage`).
Per-binding emitters land in a follow-up PR.

Design rationale: see `.notes/api-idl-rfc.md`.

## Quick start

```bash
# bootstrap venv and install deps
bash tools/codegen/setup.sh

# regenerate the golden header from the spec
bash tools/codegen/scripts/regenerate.sh

# run all checks (CI-equivalent)
bash tools/codegen/scripts/check.sh
```

## What lives where

```
tools/codegen/
├── README.md
├── requirements.txt              # libclang dep (PyPI package)
├── setup.sh                      # creates .venv, installs deps
├── scripts/
│   ├── check.sh                  # CI-equivalent local check
│   └── regenerate.sh             # regenerate golden output
├── flox_codegen/
│   ├── ir.py                     # IR dataclasses (Module, Function, ...)
│   ├── extractor.py              # libclang AST → IR
│   ├── emit_capi.py              # IR → flox_capi.h text
│   ├── check_signatures.py       # diff two C headers (sigs only)
│   └── cli.py                    # python -m flox_codegen.cli ...
├── golden/
│   └── flox_capi.h              # committed reference output
├── tests/
│   ├── test_annotation_parser.py
│   ├── test_extractor.py
│   ├── test_emit_capi.py
│   └── test_check_signatures.py
└── .venv/                        # gitignored

include/flox/capi/
├── flox_export.h                 # FLOX_EXPORT(...) macro definition
├── flox_capi_spec.hpp            # the IDL — annotated declarations
├── flox_capi.h                   # the live hand-written C header (untouched in T013)
└── ...
```

## How a function flows from spec to generated header

1. **Author** annotates a free function in `flox_capi_spec.hpp`:

   ```cpp
   FLOX_EXPORT(group="indicator")
   void flox_indicator_ema(const double* input, size_t len, size_t period,
                           double* output);
   ```

2. **Codegen** parses the spec via libclang; the `[[clang::annotate(...)]]`
   attribute injected by the macro carries the metadata into the AST.
   `flox_codegen.extractor` collects every annotated declaration into an IR
   `Module`.

3. **Emit** walks the IR and produces `golden/flox_capi.h`. Functions are
   grouped by their `group=` annotation; output ordering and formatting are
   deterministic.

4. **Check** diffs the generated header against the live `flox_capi.h`
   parsed by libclang. Mismatches in name/return-type/param-types fail CI;
   functions in the live header but not yet in the spec are reported as
   informational coverage gaps (the prototype legitimately covers a subset).

## Annotation vocabulary (initial)

`FLOX_EXPORT(...)` accepts these keys (see `include/flox/capi/flox_export.h`
for the source-side reference):

| key                    | meaning                                                      |
|------------------------|--------------------------------------------------------------|
| `c_name="..."`         | C symbol name (default: spec declaration's name)             |
| `group="..."`          | section in the emitted header                                |
| `handle="..."`         | declaration creates an opaque-handle typedef                 |
| `on_handle="..."`      | method takes an existing handle as first arg                 |
| `return_kind="..."`    | hint for return convention (`order_id`, `bool_u8`, …)        |
| `pointer_out_wrapper`  | auto-emit `*_p` Codon/QuickJS variant                        |
| `callback_bundle`      | this struct is a callback bundle                             |
| `internal_only`        | suppress codegen output                                      |

Bare flags (no `=value`) are parsed as flag-keys with empty string values.
Unknown keys are kept on the IR for emitters to consume.

## Adding a new function to the slice

1. Add an annotated declaration to `flox_capi_spec.hpp`.
2. Run `tools/codegen/scripts/regenerate.sh`.
3. Commit `flox_capi_spec.hpp` and `tools/codegen/golden/flox_capi.h`
   together. CI will reject drift between them.

## Adding a new emitter (T014 scope)

Each emitter consumes the same `ir.Module` and produces a single artifact.
Drop a new file under `flox_codegen/`:

```python
# emit_pyi.py
from . import ir

def emit(module: ir.Module) -> str:
    out = []
    for fn in module.functions:
        out.append(f"def {fn.name}(...) -> {map_type(fn.return_type)}: ...")
    return "\n".join(out)
```

Wire it in `cli.py` as a new subcommand. The IR is the contract — emitters
don't know about libclang.

## CI gate

`.github/workflows/codegen.yml` runs `tools/codegen/scripts/check.sh` on
every push and PR. Failure modes:

- **Golden drift** — spec edited without regenerating `golden/flox_capi.h`.
  Fix: run `regenerate.sh`, commit.
- **Signature mismatch** — codegen output disagrees with live `flox_capi.h`
  on a function the spec covers. Fix: align spec or live header.
- **Tests** — anything in `tools/codegen/tests/` fails.

The `--require-full-coverage` flag is **off** during the slice prototype.
T014 will turn it on once the spec covers the full live header.

## Known limitations (resolved in T014)

- Only `flox_capi.h` is emitted; `.pyi`, `.d.ts`, Codon, and llms.txt
  emitters land in T014.
- The slice is intentionally minimal — see the RFC for the full target list.
- libclang version is locked via the `libclang` PyPI package's pinned
  version (`requirements.txt`); CI installs from the same pin.
- Templates (e.g. `IndicatorGraph`, `MultiTimeframeAggregator`) are not yet
  reachable through the spec — they require an explicit instantiation
  convention, deferred to T014.
