# Contributing

Guidelines for contributing to lrvx.

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

lrvx welcomes contributions in these areas:

- **Features** — New functionality and capabilities
- **Connectors** — New exchange integrations
- **Bug fixes** — Correctness improvements
- **Performance** — Latency and throughput optimizations
- **Documentation** — Examples, tutorials, clarifications
- **Tests** — Improved coverage

## Build Options Reference

Artefact selection uses the `LRVX_BUILD_*` prefix; behaviour switches keep `LRVX_ENABLE_*`.

| Option | Default | Description |
|--------|---------|-------------|
| `LRVX_BUILD_TESTS` | `OFF` | Build unit tests |
| `LRVX_BUILD_BENCHMARKS` | `OFF` | Build benchmark binaries |
| `LRVX_BUILD_DEMO` | `OFF` | Build demo application |
| `LRVX_BUILD_TOOLS` | `OFF` | Build command-line tools (preagg_bars, etc.) |
| `LRVX_BUILD_PYTHON` | `OFF` | Build Python bindings |
| `LRVX_BUILD_NODE` | `OFF` | Build Node.js bindings (out-of-tree via npm; CMake does not invoke npm) |
| `LRVX_BUILD_CAPI` | `OFF` | Build the C API shared library (implies `LRVX_ENABLE_BACKTEST`) |
| `LRVX_BUILD_CODON` | `OFF` | Build Codon strategy support (requires `LRVX_BUILD_CAPI=ON`) |
| `LRVX_BUILD_QUICKJS` | `OFF` | Build QuickJS strategy support (requires `LRVX_BUILD_CAPI=ON`) |
| `LRVX_BUILD_CONNECTORS` | `OFF` | Build the native exchange connectors module |
| `LRVX_CONNECTORS` | `""` | Semicolon-separated venue subset; empty = every venue under `connectors/src/` |
| `LRVX_ENABLE_BACKTEST` | `OFF` | Build backtest module (simulated execution) |
| `LRVX_ENABLE_LZ4` | `ON` | Enable LZ4 compression for binary logs |
| `LRVX_ENABLE_CPU_AFFINITY` | `OFF` | Enable CPU affinity and NUMA functionality |
| `LRVX_ENABLE_TRACY` | `OFF` | Enable Tracy profiler integration |
| `LRVX_ENABLE_DEV_SETUP` | `OFF` | Install pre-commit hook automatically |
| `LRVX_NATIVE` | `ON` | Compile Release with `-march=native`. Turn OFF for distributable artifacts |

Enable with:

```bash
cmake .. -DLRVX_BUILD_TESTS=ON -DLRVX_BUILD_BENCHMARKS=ON
```

## See Also

- [Quickstart](../tutorials/quickstart.md) — Build and run lrvx
- [Architecture](../explanation/architecture.md) — Understand the codebase
