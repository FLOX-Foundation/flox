# Getting Started

This guide will help you build, test, and install Flox on your machine.

## Prerequisites

* C++20 or later (e.g. GCC 13 or Clang 16+)
* CMake 3.22+
* Git
* Linux (recommended)
* GoogleTest and Google Benchmark
* `clang-format` 18.1.8 (for development)


## Clone and Build

```bash
git clone https://github.com/eeiaao/flox.git
cd flox
mkdir build && cd build
cmake ..
make -j$(nproc)
```


## Install Dependencies

If GoogleTest and Google Benchmark are not installed system-wide:

```bash
# GoogleTest
git clone --depth=1 https://github.com/google/googletest.git
cmake -B gtest-build -S googletest
cmake --build gtest-build --target gtest gtest_main gmock gmock_main
sudo cmake --install gtest-build

# Google Benchmark
git clone --depth=1 https://github.com/google/benchmark.git
cmake -B benchmark-build -S benchmark -DCMAKE_BUILD_TYPE=Release -DBENCHMARK_DOWNLOAD_DEPENDENCIES=ON
cmake --build benchmark-build -j$(nproc)
sudo cmake --install benchmark-build
```

## clang-format Setup

We use `clang-format` 18.x to enforce consistent style. Install it with:

```bash
sudo apt install -y wget gnupg lsb-release software-properties-common
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 18
sudo apt install -y clang-format-18
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 100
```

## Build Options

Flox supports optional components controlled via CMake flags:

| Option             | Default | Description                |
| ------------------ | ------- | -------------------------- |
| `ENABLE_TESTS`     | `OFF`   | Build unit tests           |
| `ENABLE_BENCHMARK` | `OFF`   | Build benchmark binaries   |
| `ENABLE_DEMO`      | `OFF`   | Build the demo application |

To enable them:

```bash
cmake .. -DENABLE_TESTS=ON -DENABLE_BENCHMARK=ON -DENABLE_DEMO=ON
```

## Run Tests

From the `build` directory:

```bash
ctest --output-on-failure
```

## Run Benchmarks

```bash
./benchmarks/nlevel_order_book_benchmark
```

Or any other binary in `benchmarks/`.

## Install System-Wide

```bash
sudo make install
```

## Code Style and Contribution

* All contributions go through pull requests
* Use existing naming and directory conventions
* Add tests, benchmarks, and documentation where appropriate

A `.clang-format` file is provided. A `pre-commit` hook is installed automatically during CMake configuration. It formats all changed `.cpp` and `.h` files.

To install it manually:

```bash
cp scripts/pre-commit .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
```

## Using Flox in Your Project

Flox is a low-latency infrastructure library.
It is suitable for building:

* HFT engines
* Backtesters and simulators
* Custom execution pipelines
* Signal routers and adapters

All components are modular, testable, and can be used independently.
