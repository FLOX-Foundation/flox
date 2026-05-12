# Build feature flags

CMake options for tailoring a FLOX build to a specific use case. Two prefixes, by purpose:

- `FLOX_ENABLE_*` — capabilities of the core library (backtest module, LZ4 compression, Tracy profiler, CPU affinity).
- `FLOX_BUILD_*` — optional build artefacts (binding wheels, demo, tools, tests, benchmarks, connectors).

Most options default to OFF unless noted, so a bare `cmake -B build` produces only the core C++ static library. Two exceptions are flagged explicitly in the tables below (`FLOX_NATIVE`, `FLOX_ENABLE_LZ4`).

A third prefix gates compiler/portability flags rather than features:

- `FLOX_NATIVE` — controls whether Release builds target the build host's exact ISA (`-march=native`) or a portable baseline. Default ON for fastest local builds; flip OFF when shipping artefacts to machines whose CPUs you do not control (e.g. `flox-py` wheels do this automatically).

## Capabilities

| Flag | Default | Effect |
|---|---|---|
| `FLOX_ENABLE_BACKTEST` | OFF | Compile `src/backtest/` into the core library. Required for `BacktestRunner`, `WalkForwardRunner`, `GridSearch`. |
| `FLOX_ENABLE_LZ4` | **ON** | Link LZ4 for replay-log compression. Tries `find_package(lz4 CONFIG)` first, then `find_library`, then pkg-config, then a vendored LZ4 source via FetchContent — so the dependency self-resolves on every supported platform. Set to OFF only if you specifically need an LZ4-free build. |
| `FLOX_ENABLE_TRACY` | OFF | Link the Tracy profiler client. Adds runtime instrumentation; OFF in production builds. |
| `FLOX_ENABLE_CPU_AFFINITY` | OFF | Compile pthread / NUMA-aware affinity helpers. Linux only; on macOS / Windows the calls become no-ops. **Warning:** can hurt perf on busy or shared systems — only use on isolated dedicated hardware. |
| `FLOX_ENABLE_DEV_SETUP` | OFF | Install the project's pre-commit hook into `.git/hooks/` at configure time. Developer-only. |

## Artefacts

| Flag | Default | Effect | Depends on |
|---|---|---|---|
| `FLOX_BUILD_TESTS` | OFF | Build the GoogleTest suite under `tests/`. | `find_package(GTest)` |
| `FLOX_BUILD_BENCHMARKS` | OFF | Build the Google Benchmark suite under `benchmarks/`. | benchmark, optionally `FLOX_ENABLE_BACKTEST` |
| `FLOX_BUILD_DEMO` | OFF | Build the `flox_demo` executable. | optionally `FLOX_ENABLE_BACKTEST` |
| `FLOX_BUILD_TOOLS` | OFF | Build the CLI tools under `tools/`. | optionally `FLOX_ENABLE_BACKTEST` |
| `FLOX_BUILD_PYTHON` | OFF | Build `flox_py` pybind11 binding. | pybind11, Python 3.10+ |
| `FLOX_BUILD_NODE` | OFF | Documentation flag. The Node addon is built out-of-tree by `npm run build`; this flag exists for parity in the CI matrix. CMake itself does not invoke npm. | npm, Node 18+ |
| `FLOX_BUILD_CAPI` | OFF | Build the `libflox_capi.so` shared library — the integration point for any other language. Forces the static `flox` library to be position-independent. | — |
| `FLOX_BUILD_CODON` | OFF | Build the Codon strategy support layer under `codon/`. | `FLOX_BUILD_CAPI=ON`, Codon compiler |
| `FLOX_BUILD_QUICKJS` | OFF | Build the embedded JS strategy runtime. | `FLOX_BUILD_CAPI=ON` |
| `FLOX_BUILD_CONNECTORS` | OFF | Build the native exchange connectors module under `connectors/`. | OpenSSL / zlib / libcurl, plus `ixwebsocket` and `simdjson` via FetchContent |

### `FLOX_CONNECTORS` selector

When `FLOX_BUILD_CONNECTORS=ON`, the cache string `FLOX_CONNECTORS` chooses which venues to compile.

```bash
cmake -B build -DFLOX_BUILD_CONNECTORS=ON \
                -DFLOX_CONNECTORS="bybit;bitget"
```

The empty string (the default) means "all venues currently in `connectors/src/`". Listing an unknown venue produces a configure-time error with the available set.

## Compiler options

| Flag | Default | Effect |
|---|---|---|
| `FLOX_NATIVE` | **ON** | Add `-march=native` to Release builds. Fastest on the build host, but the resulting `.so` / `.a` will fault with SIGILL on any CPU lacking an instruction the build host had (e.g. AVX-512 → consumer x86). When OFF on x86_64, falls back to `-march=x86-64-v3` (AVX2/BMI2/FMA baseline, supported since ~2015); on arm64 the compiler default is used. Distribution paths (wheels, prebuilt binaries) must build with `FLOX_NATIVE=OFF`. |

## Recommended configurations

```bash
# Research / Python-only
cmake -B build -DFLOX_BUILD_PYTHON=ON -DFLOX_ENABLE_BACKTEST=ON

# Production trading service (Python + Node + connectors, narrowed)
cmake -B build -DFLOX_BUILD_PYTHON=ON \
                -DFLOX_BUILD_NODE=ON \
                -DFLOX_BUILD_CONNECTORS=ON \
                -DFLOX_CONNECTORS="bybit;bitget" \
                -DFLOX_ENABLE_BACKTEST=ON \
                -DFLOX_ENABLE_LZ4=ON

# Minimal CI gate — just confirm the core library compiles
cmake -B build -DFLOX_BUILD_TESTS=ON

# Full developer build (everything)
cmake -B build -DFLOX_BUILD_TESTS=ON -DFLOX_BUILD_BENCHMARKS=ON \
                -DFLOX_BUILD_DEMO=ON -DFLOX_BUILD_TOOLS=ON \
                -DFLOX_BUILD_PYTHON=ON -DFLOX_BUILD_CAPI=ON \
                -DFLOX_BUILD_CODON=ON -DFLOX_BUILD_QUICKJS=ON \
                -DFLOX_BUILD_CONNECTORS=ON \
                -DFLOX_ENABLE_BACKTEST=ON -DFLOX_ENABLE_LZ4=ON \
                -DFLOX_ENABLE_CPU_AFFINITY=ON
```

## Deprecated names

The eight artefact options were renamed from `FLOX_ENABLE_*` to `FLOX_BUILD_*` during a build-flag cleanup. The legacy names continue to work as aliases for one release cycle:

| Legacy | Current | Status |
|---|---|---|
| `FLOX_ENABLE_BENCHMARKS` | `FLOX_BUILD_BENCHMARKS` | deprecated |
| `FLOX_ENABLE_TESTS` | `FLOX_BUILD_TESTS` | deprecated |
| `FLOX_ENABLE_DEMO` | `FLOX_BUILD_DEMO` | deprecated |
| `FLOX_ENABLE_TOOLS` | `FLOX_BUILD_TOOLS` | deprecated |
| `FLOX_ENABLE_PYTHON` | `FLOX_BUILD_PYTHON` | deprecated |
| `FLOX_ENABLE_CAPI` | `FLOX_BUILD_CAPI` | deprecated |
| `FLOX_ENABLE_CODON` | `FLOX_BUILD_CODON` | deprecated |
| `FLOX_ENABLE_QUICKJS` | `FLOX_BUILD_QUICKJS` | deprecated |

Passing `-DFLOX_ENABLE_PYTHON=ON` still flips `FLOX_BUILD_PYTHON=ON` and emits a one-line deprecation warning. Migrate downstream `cmake` invocations and tooling that parses `cmake -L` to the new names; the old names will be removed in a later release cycle.

## FAQ

**Why is everything OFF by default?**
A backtest-only Python user shouldn't pay the cost of the QuickJS runtime, nor a Codon developer the CCXT dependency tree. Each consumer opts in to what they need. The CI matrix in `build-matrix.yml` exercises every option in isolation so a default-OFF assumption can't silently rot.

**Can I add a new option?**
Yes. Decide which prefix fits — capability of the core library is `ENABLE`, optional artefact is `BUILD`. Add the option, the conditional `add_subdirectory` (or whatever the artefact needs), a row in the table above, and a matrix config in `build-matrix.yml` that exercises it in isolation.

**Why no aggregate `FLOX_FULL=ON` flag?**
Aggregate flags drift. Today "full" means `BUILD_PYTHON + BUILD_NODE + BUILD_CAPI + …`; if a new artefact lands, downstream that pinned `FLOX_FULL` for "everything" suddenly drags in something they didn't ask for. The `full-build` config in the CI matrix already serves the "does everything compile together" purpose without giving users a foot-gun. If you want a one-line preset for your own machine, [CMakePresets.json](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) is the standard escape hatch.
