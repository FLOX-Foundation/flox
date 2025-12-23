# Continuous Integration

FLOX uses GitHub Actions for continuous integration. The CI pipeline runs on every push and pull request to ensure code quality and cross-platform compatibility.

## CI Matrix

| Job | Platform | Compiler | Build Type | Description |
|-----|----------|----------|------------|-------------|
| `format-check` | Ubuntu | - | - | Validates code formatting with clang-format |
| `linux-gcc` | Ubuntu 24.04 | GCC 14 | Release | Main Linux build |
| `linux-clang` | Ubuntu 24.04 | Clang 18 + libc++ | Release | Clang with libc++ standard library |
| `sanitizers` | Ubuntu 24.04 | GCC 14 | Debug | ASan and UBSan for memory/UB detection |
| `macos` | macOS latest | Apple Clang | Release | macOS compatibility |
| `windows-msvc` | Windows latest | MSVC | Release | Windows with MSVC toolchain |
| `windows-clang-cl` | Windows latest | Clang-CL | Release | Windows with Clang frontend |
| `affinity-tests` | Ubuntu 24.04 | GCC 14 | Release | Weekly CPU affinity tests (scheduled) |

## Build Configuration

All builds use **Release mode only**. Debug builds are intentionally excluded because:

1. Sanitizers (ASan, UBSan) catch memory and undefined behavior issues better than Debug assertions
2. Release mode tests the actual production code path
3. Reduces CI time and resource usage

## Sanitizers

The `sanitizers` job runs with both AddressSanitizer and UndefinedBehaviorSanitizer:

```yaml
matrix:
  sanitizer: [address, undefined]
```

| Sanitizer | Detects |
|-----------|---------|
| AddressSanitizer | Buffer overflows, use-after-free, memory leaks |
| UndefinedBehaviorSanitizer | Signed overflow, null pointer dereference, alignment issues |

Sanitizer options:

```
ASAN_OPTIONS: detect_leaks=1:abort_on_error=1
UBSAN_OPTIONS: print_stacktrace=1:halt_on_error=1
```

## Weekly Affinity Tests

CPU affinity tests run weekly (Sunday 3:00 UTC) because:

- Require `libnuma-dev` which isn't available on all platforms
- GitHub runners don't have isolated cores for reliable affinity testing
- Tests verify the affinity API works, not that it improves performance

## Running CI Locally

### Format Check

```bash
./scripts/check-format.sh
```

### Build with GCC

```bash
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_BUILD_TYPE=Release \
  -DFLOX_ENABLE_TESTS=ON \
  -DFLOX_ENABLE_BENCHMARKS=ON \
  -DFLOX_ENABLE_DEMO=ON \
  -DFLOX_ENABLE_LZ4=ON \
  -DFLOX_ENABLE_BACKTEST=ON

cmake --build build -j$(nproc)
ctest --output-on-failure --test-dir build
```

### Build with Sanitizers

```bash
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFLOX_ENABLE_TESTS=ON

cmake --build build
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 ctest --output-on-failure --test-dir build
```

## Dependencies

### Ubuntu

```bash
sudo apt-get install cmake ninja-build liblz4-dev g++-14
```

### macOS

```bash
brew install cmake ninja lz4
```

### Windows

Uses vcpkg for dependencies:

```powershell
vcpkg install lz4:x64-windows gtest:x64-windows benchmark:x64-windows
```

## See Also

- [Configuration](configuration.md) - Runtime configuration
