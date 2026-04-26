# FLOX

**High-performance trading framework with bindings for Python, Node.js, Codon, and C++.**

[![GitHub](https://img.shields.io/badge/GitHub-FLOX--Foundation%2Fflox-blue?logo=github)](https://github.com/FLOX-Foundation/flox)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](https://github.com/FLOX-Foundation/flox/blob/main/LICENSE)

---

## Getting started

Pick your language:

| | |
|---|---|
| [**Python quickstart**](tutorials/python-quickstart.md) | Indicators, strategies, and backtesting from Python |
| [**Node.js quickstart**](tutorials/node-quickstart.md) | Same API from JavaScript/TypeScript |
| [**C++ quickstart**](tutorials/quickstart.md) | Build FLOX and run the demo |
| [**Architecture**](explanation/architecture.md) | How the components fit together |

---

## Quick example

=== "Python"

    ```python
    import flox_py as flox

    ema = flox.EMA(20)
    rsi = flox.RSI(14)

    for price in prices:
        e = ema.update(price)
        r = rsi.update(price)
        if e is not None and r is not None:
            print(f"EMA {e:.2f}  RSI {r:.1f}")
    ```

=== "Node.js"

    ```javascript
    const flox = require('./build/node');

    const ema = new flox.EMA(20);
    const rsi = new flox.RSI(14);

    for (const price of prices) {
        const e = ema.update(price);
        const r = rsi.update(price);
        if (e !== null && r !== null) {
            console.log(`EMA ${e.toFixed(2)}  RSI ${r.toFixed(1)}`);
        }
    }
    ```

=== "C++"

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
        void stop() override  { _running = false; }

    private:
        flox::SymbolId _targetSymbol;
        bool _running = false;
    };
    ```

---

## Language bindings

| Language | Guide | Reference |
|---|---|---|
| Python | [Python](bindings/python.md) | [reference](reference/python/index.md) |
| Node.js | [Node.js](bindings/node.md) | [reference](reference/node/index.md) |
| Codon | [Codon](bindings/codon.md) | [reference](reference/codon/index.md) |
| JavaScript (embedded) | [JavaScript](bindings/javascript.md) | [reference](reference/quickjs/index.md) |
| C API | [C API](bindings/capi.md) | [reference](reference/api/capi/flox_capi.md) |

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
| Lock-free event delivery | Disruptor-style ring buffers, busy-spin consumers |
| Zero-allocation hot path | Pre-allocated pools, no heap allocation in market data callbacks |
| CPU affinity support | Pin threads to isolated cores |
| Multi-exchange trading | [CEX coordination](reference/api/cex/index.md) with aggregation and smart routing |
| Binary replay system | Record live data, replay for backtesting |
| Grid search optimization | Parallel parameter optimization with mmap-based bar storage |
| Multi-language bindings | Python, Node.js, Codon, JavaScript, C API |
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

## License

MIT License
