# Test gating

The build matrix has a `tests-only` configuration that builds with
`FLOX_BUILD_TESTS=ON` but no other flags. If a test exec depends on
backtest-only sources (`flox/backtest/...`) or C API symbols
(`flox/capi/...`) without sitting under the matching CMake gating
block, the linker fails on that config — the test cpp compiles fine
but cannot find the symbols it needs.

`scripts/check_test_gating.py` runs in CI (verify-docs-current) and
fails any PR whose test additions slip through this gate.

## The rule

| Header prefix       | Required CMake flag      |
|---------------------|--------------------------|
| `flox/backtest/...` | `FLOX_ENABLE_BACKTEST`   |
| `flox/capi/...`     | `FLOX_BUILD_CAPI`        |

Implications: `FLOX_BUILD_CAPI` implies `FLOX_ENABLE_BACKTEST`,
`FLOX_BUILD_CODON` and `FLOX_BUILD_QUICKJS` both imply
`FLOX_BUILD_CAPI`. The checker walks these so a test under
`if(FLOX_BUILD_QUICKJS)` is treated as having `FLOX_BUILD_CAPI` and
`FLOX_ENABLE_BACKTEST` active.

## What to do when the gate fails

The error tells you which flag is missing for which test. Move
`add_flox_test(test_name)` into the matching `if(...)` block in
`tests/CMakeLists.txt`. Example fix from W15-T020:

```cmake
# Wrong — test_live_queue_position needs FLOX_ENABLE_BACKTEST.
add_flox_test(test_live_queue_position)

# Right — inside the backtest gate.
if(FLOX_ENABLE_BACKTEST)
  ...
  add_flox_test(test_live_queue_position)
endif()
```

## Running locally

```
python3 scripts/check_test_gating.py
```

The script prints a coverage map of every test target with its
declared CMake gating and its needed-from-includes set. A clean run
ends with `OK — N test targets, gating consistent.`

## Scope

- Direct `#include` lines only. Transitive includes through engine
  headers are not chased — false negatives are possible if a public
  engine header internally pulls in a backtest header, but those
  cases also show up as link failures during `tests-only` CI and
  bubble up the same way.
- No auto-fix. The check surfaces the gap; the PR author moves the
  test into the right block.
- The `FLAG_IMPLIES` map at the top of the script mirrors the
  hard-prereq guards in the root `CMakeLists.txt`. Keep them in
  sync when adding a new flag dependency.
