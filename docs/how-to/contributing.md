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

- **Connectors** — New exchange integrations
- **Bug fixes** — Correctness improvements
- **Performance** — Latency and throughput optimizations
- **Documentation** — Examples, tutorials, clarifications
- **Tests** — Improved coverage

## Build Options Reference

| Option | Default | Description |
|--------|---------|-------------|
| `FLOX_ENABLE_TESTS` | `OFF` | Build unit tests |
| `FLOX_ENABLE_BENCHMARKS` | `OFF` | Build benchmark binaries |
| `FLOX_ENABLE_DEMO` | `OFF` | Build the demo application |
| `FLOX_ENABLE_LZ4` | `OFF` | Enable LZ4 compression for replay |
| `FLOX_ENABLE_CPU_AFFINITY` | `OFF` | Enable CPU affinity (isolated systems only) |

Enable with:

```bash
cmake .. -DFLOX_ENABLE_TESTS=ON -DFLOX_ENABLE_BENCHMARKS=ON
```

## See Also

- [Quickstart](../tutorials/quickstart.md) — Build and run FLOX
- [Architecture](../explanation/architecture.md) — Understand the codebase
