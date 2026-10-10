# Quickstart

Build lrvx and run the demo application in 5 minutes.

## 1. Clone and Build

```bash
git clone https://github.com/lrvx/lrvx.git
cd lrvx

mkdir build && cd build
cmake .. -DLRVX_BUILD_DEMO=ON
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
```

## 2. Run the Demo

```bash
./demo/lrvx_demo
```

Logging is switched off for the duration of the run, so nothing
prints until the 30 seconds are up. Then it re-enables logging and
dumps the latency report — one line per label, with `count`, `mean`,
`p50`, `p95` and `max` in nanoseconds:

```
demo finished
[latency] bus_publish | count=... mean=...ns p50=...ns p95=...ns max=...ns
[latency] strategy_onTrade | count=... mean=...ns p50=...ns p95=...ns max=...ns
[latency] execution_onOrderFilled | count=... mean=...ns p50=...ns p95=...ns max=...ns
[latency] end_to_end | count=... mean=...ns p50=...ns p95=...ns max=...ns
```

There is no p99 — the collector reports p95.

## 3. What the Demo Does

The demo creates a complete trading system:

```mermaid
flowchart LR
    subgraph Connector
        DC[DemoConnector<br/>generates fake data]
    end

    subgraph Bus["Event Bus"]
        TB[TradeBus]
        BB[BookBus]
    end

    subgraph Strategy
        DS[DemoStrategy<br/>reacts to trades]
    end

    subgraph Execution
        EB[ExecutionBus]
    end

    DC --> TB
    DC --> BB
    TB --> DS
    BB --> DS
    DS --> EB
```

1. **DemoConnector** generates fake trades and order book updates
2. Events flow through **TradeBus** and **BookUpdateBus** (Disruptor-style ring buffers)
3. **DemoStrategy** receives events and submits orders
4. Orders flow through **OrderExecutionBus** to an execution tracker

## 4. Build Options

The most common ones. `LRVX_ENABLE_*` gates capabilities of the core
library, `LRVX_BUILD_*` gates optional artefacts.

| Option | Default | Description |
|--------|---------|-------------|
| `LRVX_BUILD_DEMO` | OFF | Build the demo application |
| `LRVX_BUILD_TESTS` | OFF | Build unit tests |
| `LRVX_BUILD_BENCHMARKS` | OFF | Build performance benchmarks |
| `LRVX_BUILD_PYTHON` | OFF | Build the `lrvx` pybind11 binding |
| `LRVX_BUILD_CAPI` | OFF | Build `liblrvx_capi.so` (implies `LRVX_ENABLE_BACKTEST`) |
| `LRVX_ENABLE_BACKTEST` | OFF | Build backtest module (simulated execution) |
| `LRVX_ENABLE_LZ4` | ON | Enable LZ4 compression for replay |
| `LRVX_ENABLE_CPU_AFFINITY` | OFF | Enable CPU pinning (requires libnuma) |
| `LRVX_ENABLE_TRACY` | OFF | Enable Tracy profiler integration |

The full set — `LRVX_BUILD_TOOLS`, `LRVX_BUILD_NODE`, `LRVX_BUILD_CODON`,
`LRVX_BUILD_QUICKJS`, `LRVX_BUILD_CONNECTORS`, `LRVX_CONNECTORS`,
`LRVX_NATIVE`, `LRVX_ENABLE_DEV_SETUP` — plus the deprecated
`LRVX_ENABLE_*` artefact aliases is in
[Build feature flags](../build/feature-flags.md).

Example with multiple options:

```bash
cmake .. \
  -DLRVX_BUILD_DEMO=ON \
  -DLRVX_BUILD_TESTS=ON \
  -DLRVX_ENABLE_LZ4=ON \
  -DCMAKE_BUILD_TYPE=Release
```

## 5. Verify Installation

Run the tests to verify everything works:

```bash
cmake .. -DLRVX_BUILD_TESTS=ON
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
ctest --output-on-failure
```

## Next Steps

- [First Strategy](first-strategy.md) — Write your own trading strategy
- [Architecture Overview](../explanation/architecture.md) — Understand how components fit together
