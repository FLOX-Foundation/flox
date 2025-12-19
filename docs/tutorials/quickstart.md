# Quickstart

Build FLOX and run the demo application in 5 minutes.

## 1. Clone and Build

```bash
git clone https://github.com/FLOX-Foundation/flox.git
cd flox

mkdir build && cd build
cmake .. -DFLOX_ENABLE_DEMO=ON
make -j$(nproc)
```

## 2. Run the Demo

```bash
./demo/flox_demo
```

The demo runs for 30 seconds, then prints latency statistics:

```
[demo] price spike starting
[demo] price spike starting
demo finished
=== Latency Report ===
BusPublish:       p50=123ns  p99=456ns  max=1.2µs
StrategyOnTrade:  p50=89ns   p99=234ns  max=890ns
```

## 3. What the Demo Does

The demo creates a complete trading system:

```
┌─────────────┐     ┌───────────┐     ┌──────────────┐
│ DemoConnector│────▶│ TradeBus  │────▶│ DemoStrategy │
│ (generates   │     │ BookBus   │     │ (reacts to   │
│  fake data)  │     └───────────┘     │  trades)     │
└─────────────┘                        └──────────────┘
                                              │
                                              ▼
                                       ┌──────────────┐
                                       │ ExecutionBus │
                                       └──────────────┘
```

1. **DemoConnector** generates fake trades and order book updates
2. Events flow through **TradeBus** and **BookUpdateBus** (Disruptor-style ring buffers)
3. **DemoStrategy** receives events and submits orders
4. Orders flow through **OrderExecutionBus** to an execution tracker

## 4. Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `FLOX_ENABLE_DEMO` | OFF | Build the demo application |
| `FLOX_ENABLE_TESTS` | OFF | Build unit tests |
| `FLOX_ENABLE_BENCHMARKS` | OFF | Build performance benchmarks |
| `FLOX_ENABLE_BACKTEST` | OFF | Build backtest module (simulated execution) |
| `FLOX_ENABLE_LZ4` | OFF | Enable LZ4 compression for replay |
| `FLOX_ENABLE_CPU_AFFINITY` | OFF | Enable CPU pinning (requires libnuma) |
| `FLOX_ENABLE_TRACY` | OFF | Enable Tracy profiler integration |

Example with multiple options:

```bash
cmake .. \
  -DFLOX_ENABLE_DEMO=ON \
  -DFLOX_ENABLE_TESTS=ON \
  -DFLOX_ENABLE_LZ4=ON \
  -DCMAKE_BUILD_TYPE=Release
```

## 5. Verify Installation

Run the tests to verify everything works:

```bash
cmake .. -DFLOX_ENABLE_TESTS=ON
make -j$(nproc)
ctest --output-on-failure
```

## Next Steps

- [First Strategy](first-strategy.md) — Write your own trading strategy
- [Architecture Overview](../explanation/architecture.md) — Understand how components fit together
