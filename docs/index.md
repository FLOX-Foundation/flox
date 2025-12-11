# FLOX

**High-performance C++ framework for building trading systems.**

[![GitHub](https://img.shields.io/badge/GitHub-FLOX--Foundation%2Fflox-blue?logo=github)](https://github.com/FLOX-Foundation/flox)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](https://github.com/FLOX-Foundation/flox/blob/main/LICENSE)

---

## Getting Started

| | |
|---|---|
| [**Quickstart**](tutorials/quickstart.md) | Build FLOX and run the demo in 5 minutes |
| [**First Strategy**](tutorials/first-strategy.md) | Write and run your first trading strategy |
| [**Architecture**](explanation/architecture.md) | Understand how components work together |
| [**API Reference**](reference/README.md) | Complete technical documentation |

---

## Documentation

| Section | Description |
|---------|-------------|
| [Tutorials](tutorials/README.md) | Step-by-step lessons for new users |
| [How-To Guides](how-to/README.md) | Solutions for specific problems |
| [Explanation](explanation/README.md) | Architecture and design concepts |
| [Reference](reference/README.md) | API specifications |

---

## Features

| Feature | Description |
|---------|-------------|
| Lock-free event delivery | Disruptor-style ring buffers for minimal latency |
| Zero-allocation hot path | Pre-allocated pools, no heap allocation during trading |
| CPU affinity support | Pin threads to isolated cores |
| Binary replay system | Record live data, replay for backtesting |
| Type-safe primitives | Strong types for Price, Quantity, SymbolId |
| Modular architecture | Use only what you need |

---

## Requirements

| Component | Version |
|-----------|---------|
| C++ Standard | C++20 |
| Compiler | GCC 13+ or Clang 16+ |
| Build System | CMake 3.22+ |
| Platform | Linux (recommended) |

Optional: LZ4 for log compression

---

## Quick Example

```cpp
#include "flox/strategy/istrategy.h"
#include "flox/book/event/trade_event.h"

class MyStrategy : public flox::IStrategy {
public:
    void onTrade(const flox::TradeEvent& event) override {
        if (event.trade.symbol == _targetSymbol) {
            processSignal(event.trade.price);
        }
    }

    void start() override { _running = true; }
    void stop() override { _running = false; }

private:
    flox::SymbolId _targetSymbol;
    bool _running = false;
};
```

[Full tutorial →](tutorials/first-strategy.md)

---

## License

MIT License
