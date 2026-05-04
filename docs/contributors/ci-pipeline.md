# CI Pipeline

Mental model for the CI workflow under [`.github/workflows/`](../../.github/workflows/) — what runs in what order, what fails fast, and what to regenerate locally before pushing.

## Workflow shape

```
┌────────────────────────────────────────────────────────────────────┐
│  Quick gates (parallel, ~30s each)                                  │
│                                                                     │
│  ┌──────────────┐   ┌─────────────────────┐   ┌─────────────────┐  │
│  │ format-check │   │ verify-docs-current │   │  codegen-check  │  │
│  └──────────────┘   └─────────────────────┘   └─────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
                              │
                  needs: [format-check, verify-docs-current]
                              │
                              ▼
┌────────────────────────────────────────────────────────────────────┐
│  OS build matrix (parallel, ~5–10 min each)                         │
│                                                                     │
│  ┌──────────┐ ┌───────────┐ ┌───────┐ ┌─────────────┐ ┌──────────┐ │
│  │linux-gcc │ │linux-clang│ │ macos │ │windows-msvc │ │win-clang │ │
│  └──────────┘ └───────────┘ └───────┘ └─────────────┘ └──────────┘ │
│  ┌────────────────────┐ ┌────────────────────────┐                 │
│  │sanitizers (address)│ │sanitizers (undefined)  │                 │
│  └────────────────────┘ └────────────────────────┘                 │
└────────────────────────────────────────────────────────────────────┘
```

`codegen-check` runs in a separate workflow ([`codegen.yml`](../../.github/workflows/codegen.yml)) — it's not in the dependency chain because it's already fast (~30s) and runs in parallel.

## Fast-fail wiring

Two mechanisms keep the pipeline cheap on bad pushes:

1. **`needs:` dependency.** Every OS build job in [`ci.yml`](../../.github/workflows/ci.yml) declares `needs: [format-check, verify-docs-current]`. If either quick gate fails, the multi-OS matrix is skipped — saving ~50 compute minutes (10 min × 5 jobs).
2. **`concurrency.cancel-in-progress`.** A new push to the same branch / PR cancels the in-flight workflow run for that ref. No more "two builds racing on stale code".

Both are at the workflow top:

```yaml
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  linux-gcc:
    needs: [format-check, verify-docs-current]
    ...
```

## What runs in each gate

### `format-check`

Just `./scripts/check-format.sh` — runs `clang-format --dry-run -Werror` on every C/C++/header file. Ten seconds. Failures show the exact lines that differ from the configured style; fix is `clang-format -i <file>`.

### `verify-docs-current`

Runs eight checks in sequence (each takes <5s):

| Step | What it does | If it fails |
|------|--------------|-------------|
| `gen_indicator_docs.py` | Indicator reference matches `registry.def` | Run `python3 scripts/gen_indicator_docs.py` |
| `gen_llms_txt.py --check` | `docs/llms.txt` + `llms-full.txt` match `docs/` | Run `python3 scripts/gen_llms_txt.py` |
| `check_dts_exports.py` | `node/index.d.ts` matches NAPI exports | Edit `.d.ts` to add/remove the listed names |
| `check_binding_parity.py` | pybind11/NAPI/Codon coverage matches IDL | See [parity-gate.md](parity-gate.md) |
| `check_error_codes.py` | Every error code has a doc page; pages aren't stale | Add the doc page or remove the unused code |
| `gen_api_index.py --check` | `docs/reference/python/_api_index.md` matches `.pyi` | Run `python3 scripts/gen_api_index.py` |
| `check_doc_snippets.py` | Doc snippets follow `--8<--` include pattern | Refactor inline snippets into includes |
| `sync_mcp_data.py --check` | `mcp/flox_mcp/data/` matches source | Run `python3 scripts/sync_mcp_data.py` |
| flox-mcp pytest | MCP server unit tests | Fix the broken test |

### `codegen-check` (separate workflow)

Re-runs the IDL→header/codon/markdown emitters and verifies the output matches what's committed. If you edited `flox_capi_spec.hpp` and forgot to re-run `regenerate.sh`, this fails.

### OS build matrix

Builds the full project (engine + C ABI + tests + benchmarks + Python + Node + Codon + QuickJS), runs `ctest`, runs all the integration tests, runs cross-binding parity tests (Python ↔ Node, same C++ math), and exercises example programs.

## The docs sync chain (eight scripts, in order)

The "verify-docs" gates each check that a generated artifact matches what's committed. Several of those artifacts depend on each other — regenerating one re-derives the next:

```
1. tools/codegen/scripts/regenerate.sh  → flox_capi.h, golden/, .api/, mcp data
2. cmake --build build                  → libflox_capi + Python module
3. scripts/gen_pyi_stubs.py             → .pyi from running pybind11
4. scripts/gen_api_index.py             → docs/reference/python/_api_index.md from .pyi
5. scripts/gen_llms_txt.py              → docs/llms.txt + llms-full.txt (embeds api_index)
6. scripts/sync_mcp_data.py             → mcp/flox_mcp/data/ (handled by regenerate.sh, but run manually if you skipped step 1)
7. scripts/gen_indicator_docs.py        → docs/reference/codon/indicators.md (only if you touched registry.def)
8. python3 scripts/check_binding_parity.py  → manifests vs bindings
```

**Skipping any one of those usually means a CI failure that takes ~30s to detect**, but the order matters — `gen_llms_txt.py` reads `_api_index.md`, so regenerating `_api_index.md` after generating `llms-full.txt` leaves them out of sync.

If you've touched the IDL spec or any pybind11/NAPI code, run them all in order before committing. If you've only touched a C++ engine internal that doesn't change the public surface, you don't need to.

## What fails first (debugging guide)

When CI is red, look at the topmost failed step:

- **format-check fails** → run `clang-format -i` on the listed files
- **codegen-check fails** → run `bash tools/codegen/scripts/regenerate.sh`
- **verify-docs-current fails** → look at the specific step name; run the script it names
- **OS build fails** → reproduce locally with the matching toolchain (most issues are platform-specific compile errors visible in the log)
- **OS build but only on sanitizers** → the bug exists, sanitizers caught what regular tests missed; address it, don't disable the sanitizer

The fast-fail wiring means if quick gates fail, build matrix is `SKIPPED` (gray, not red) — which is the design. If you see all build jobs gray and only one quick gate red, fix that gate; everything else will run on the next push.

## Adding a new gate

To add a new check that should block builds:

1. Add a step under `verify-docs-current` (if it's a docs/sync check) or `format-check` (if it's a static check on source).
2. Make sure the script exits non-zero on failure with a clear `::error::` annotation.
3. Verify the build jobs already `needs: [format-check, verify-docs-current]` — they do — so failures will short-circuit the matrix automatically.

If a check needs to run on every OS (e.g. exercising the platform-specific `.dylib` / `.dll`), put it in each OS build job instead. The fast-fail logic still applies — it won't even start if quick gates failed.
