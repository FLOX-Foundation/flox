# Contributing

Guidelines for contributing to FLOX.

## Code Style

A `.clang-format` file is provided in the repository. All C++ code must be formatted before committing.

### clang-format Setup

Install clang-format 18.x:

```bash
sudo apt install -y wget gnupg lsb-release software-properties-common
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 18
sudo apt install -y clang-format-18
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 100
```

### Pre-commit Hook

A `pre-commit` hook is installed automatically during CMake configuration. It formats all changed `.cpp` and `.h` files before each commit.

To install manually:

```bash
cp scripts/pre-commit .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
```

## Pull Request Process

1. Fork the repository
2. Create a feature branch from `main`
3. Make your changes
4. Ensure code is formatted (`clang-format`)
5. Add tests for new functionality
6. Run tests: `ctest --output-on-failure`
7. Submit a pull request

## Guidelines

- Use existing naming and directory conventions
- Add tests for new features
- Add benchmarks for performance-critical code
- Update documentation where appropriate
- Keep commits focused and atomic

## What to Contribute

FLOX welcomes contributions in these areas:

- **Features** — New functionality and capabilities
- **Connectors** — New exchange integrations
- **Bug fixes** — Correctness improvements
- **Performance** — Latency and throughput optimizations
- **Documentation** — Examples, tutorials, clarifications
- **Tests** — Improved coverage

## Build Options Reference

Artefact selection uses the `FLOX_BUILD_*` prefix; behaviour switches keep `FLOX_ENABLE_*`.

| Option | Default | Description |
|--------|---------|-------------|
| `FLOX_BUILD_TESTS` | `OFF` | Build unit tests |
| `FLOX_BUILD_BENCHMARKS` | `OFF` | Build benchmark binaries |
| `FLOX_BUILD_DEMO` | `OFF` | Build demo application |
| `FLOX_BUILD_TOOLS` | `OFF` | Build command-line tools (preagg_bars, etc.) |
| `FLOX_BUILD_PYTHON` | `OFF` | Build Python bindings |
| `FLOX_BUILD_NODE` | `OFF` | Build Node.js bindings (out-of-tree via npm; CMake does not invoke npm) |
| `FLOX_BUILD_CAPI` | `OFF` | Build the C API shared library (implies `FLOX_ENABLE_BACKTEST`) |
| `FLOX_BUILD_CODON` | `OFF` | Build Codon strategy support (requires `FLOX_BUILD_CAPI=ON`) |
| `FLOX_BUILD_QUICKJS` | `OFF` | Build QuickJS strategy support (requires `FLOX_BUILD_CAPI=ON`) |
| `FLOX_BUILD_CONNECTORS` | `OFF` | Build the native exchange connectors module |
| `FLOX_CONNECTORS` | `""` | Semicolon-separated venue subset; empty = every venue under `connectors/src/` |
| `FLOX_ENABLE_BACKTEST` | `OFF` | Build backtest module (simulated execution) |
| `FLOX_ENABLE_LZ4` | `ON` | Enable LZ4 compression for binary logs |
| `FLOX_ENABLE_CPU_AFFINITY` | `OFF` | Enable CPU affinity and NUMA functionality |
| `FLOX_ENABLE_TRACY` | `OFF` | Enable Tracy profiler integration |
| `FLOX_ENABLE_DEV_SETUP` | `OFF` | Install pre-commit hook automatically |
| `FLOX_NATIVE` | `ON` | Compile Release with `-march=native`. Turn OFF for distributable artifacts |

Enable with:

```bash
cmake .. -DFLOX_BUILD_TESTS=ON -DFLOX_BUILD_BENCHMARKS=ON
```

The legacy `FLOX_ENABLE_BENCHMARKS`, `FLOX_ENABLE_TESTS`, `FLOX_ENABLE_DEMO`, `FLOX_ENABLE_TOOLS`, `FLOX_ENABLE_PYTHON`, `FLOX_ENABLE_CAPI`, `FLOX_ENABLE_CODON` and `FLOX_ENABLE_QUICKJS` names still work as aliases but emit a CMake deprecation warning and will be removed.

## See Also

- [Quickstart](../tutorials/quickstart.md) — Build and run FLOX
- [Architecture](../explanation/architecture.md) — Understand the codebase
