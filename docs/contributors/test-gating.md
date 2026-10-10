# Test gating

The build matrix has a `tests-only` configuration that builds with
`LRVX_BUILD_TESTS=ON` but no other flags. If a test exec depends on
backtest-only sources (`lrvx/backtest/...`) or C API symbols
(`lrvx/capi/...`) without sitting under the matching CMake gating
block, the linker fails on that config — the test cpp compiles fine
but cannot find the symbols it needs.

`scripts/check_test_gating.py` runs in CI (verify-docs-current) and
fails any PR whose test additions slip through this gate.

## The rule

| Header prefix       | Required CMake flag      |
|---------------------|--------------------------|
| `lrvx/backtest/...` | `LRVX_ENABLE_BACKTEST`   |
| `lrvx/capi/...`     | `LRVX_BUILD_CAPI`        |

Implications: `LRVX_BUILD_CAPI` implies `LRVX_ENABLE_BACKTEST`,
`LRVX_BUILD_CODON` and `LRVX_BUILD_QUICKJS` both imply
`LRVX_BUILD_CAPI`. The checker walks these so a test under
`if(LRVX_BUILD_QUICKJS)` is treated as having `LRVX_BUILD_CAPI` and
`LRVX_ENABLE_BACKTEST` active.

## What to do when the gate fails

The error tells you which flag is missing for which test. Move
`add_lrvx_test(test_name)` into the matching `if(...)` block in
`tests/CMakeLists.txt`. Example fix:

```cmake
# Wrong — test_live_queue_position needs LRVX_ENABLE_BACKTEST.
add_lrvx_test(test_live_queue_position)

# Right — inside the backtest gate.
if(LRVX_ENABLE_BACKTEST)
  ...
  add_lrvx_test(test_live_queue_position)
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

## Sibling gate: suite discovery

`check_test_gating.py` guarantees a C++ test is *registered*.
[`check_suite_discovery.py`](../../scripts/check_suite_discovery.py) is the
Python/Node counterpart, and exists because there was none: `ci.yml` listed
test files one per named step, so 34 of 69 Python files and 9 of 21 Node files
never ran. Both suites are now invoked wholesale, and that script fails CI if a
test file would not be picked up — or if anyone re-lists files by hand.
