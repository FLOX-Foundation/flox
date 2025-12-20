# Interactive Backtest Mode

`BacktestRunner` supports an interactive mode with step-by-step execution, pause, breakpoints, and state inspection. This is useful for debugging strategies and understanding market dynamics at specific points in time.

## Quick Start

```cpp
#include "flox/backtest/backtest_runner.h"

BacktestRunner runner;
runner.setStrategy(&myStrategy);

// Set callbacks for debugging
runner.setEventCallback([](const replay::ReplayEvent& ev, const BacktestState& state) {
  std::cout << "Event " << state.eventCount << " at " << state.currentTimeNs << "\n";
});

runner.setPauseCallback([](const BacktestState& state) {
  std::cout << "Paused at event " << state.eventCount << "\n";
});

// Start in background thread (starts paused)
std::thread t([&]() { runner.start(reader); });

// Step through events one by one
runner.step();  // Process 1 event
runner.step();  // Process another

// Run until breakpoint or end
runner.resume();

t.join();
```

## Two Modes

`BacktestRunner` supports two execution modes:

### Non-Interactive Mode

Synchronous execution from start to end:

```cpp
BacktestRunner runner;
runner.setStrategy(&myStrategy);

// Blocks until complete
BacktestResult result = runner.run(reader);
```

### Interactive Mode

Async execution with pause/step control:

```cpp
BacktestRunner runner;
runner.setStrategy(&myStrategy);

// Start in background (begins paused)
std::thread t([&]() { runner.start(reader); });

// Control execution
runner.step();    // One event
runner.resume();  // Run until breakpoint/end
runner.pause();   // Pause execution
runner.stop();    // Stop completely

t.join();
```

## Execution Commands

### step()

Execute exactly one event:

```cpp
runner.step();
```

### stepUntil(mode)

Skip events until condition:

```cpp
// Skip book updates, pause on next trade
runner.stepUntil(BacktestMode::StepTrade);
```

### resume()

Run continuously until breakpoint or end:

```cpp
runner.resume();
```

### pause()

Pause execution (can resume later):

```cpp
runner.pause();
```

### stop()

Stop execution completely:

```cpp
runner.stop();
```

## Breakpoints

### Time-based

Pause at specific timestamp:

```cpp
runner.addBreakpoint(Breakpoint::atTime(1609459200000000000));  // Unix ns
```

### Event Count

Pause after N events:

```cpp
runner.addBreakpoint(Breakpoint::afterEvents(1000));
```

### Trade Count

Pause after N trades:

```cpp
runner.addBreakpoint(Breakpoint::afterTrades(100));
```

### Signal Breakpoint

Pause when strategy emits a signal:

```cpp
runner.setBreakOnSignal(true);
```

### Custom Breakpoint

Pause on custom condition:

```cpp
runner.addBreakpoint(Breakpoint::when([](const replay::ReplayEvent& ev) {
  return ev.type == replay::EventType::Trade && ev.trade.price_raw > 50000'00000000;
}));
```

### Clear Breakpoints

```cpp
runner.clearBreakpoints();
```

## State Inspection

Check current backtest state at any time:

```cpp
BacktestState state = runner.state();

std::cout << "Time: " << state.currentTimeNs << "\n"
          << "Events: " << state.eventCount << "\n"
          << "Trades: " << state.tradeCount << "\n"
          << "Book updates: " << state.bookUpdateCount << "\n"
          << "Signals: " << state.signalCount << "\n"
          << "Running: " << state.isRunning << "\n"
          << "Paused: " << state.isPaused << "\n"
          << "Finished: " << state.isFinished << "\n";
```

Convenience methods:

```cpp
bool paused = runner.isPaused();
bool done = runner.isFinished();
```

## Control Flow

```
start() ──► [Paused] ──► step() ──► [Process 1 event] ──► [Paused]
                │                                              │
                ▼                                              │
           resume() ──► [Running] ──► [Breakpoint hit] ───────►│
                │                                              │
                ▼                                              ▼
            [Finished] ◄────────────── stop() ◄──────────── pause()
```

## Example: Debug Strategy

```cpp
class DebugStrategy : public Strategy {
  void onTrade(const TradeEvent& ev) override {
    if (shouldBuy(ev)) {
      emitMarketBuy(ev.trade.symbol, Quantity::fromDouble(1.0));
    }
  }
};

int main() {
  auto reader = replay::createMultiSegmentReader("./data");

  DebugStrategy strategy;
  BacktestRunner runner;
  runner.setStrategy(&strategy);

  // Break when signal is emitted
  runner.setBreakOnSignal(true);

  std::thread t([&]() { runner.start(*reader); });

  // Run until first signal
  runner.resume();

  // Inspect state when signal was emitted
  auto state = runner.state();
  std::cout << "Signal emitted at trade #" << state.tradeCount << "\n";
  std::cout << "Time: " << state.currentTimeNs << "\n";

  // Continue running
  runner.resume();

  t.join();

  // Get results
  auto result = runner.result();
  auto stats = result.computeStats();
  std::cout << "Total trades: " << stats.totalTrades << "\n";
}
```

## Thread Safety

- `start()` must be called from a separate thread (it blocks until completion)
- `step()`, `resume()`, `pause()`, `stop()` can be called from any thread
- `state()` returns a snapshot, safe to call anytime
- Callbacks are invoked from the runner thread

## See Also

- [Backtest Runner](./backtest_runner.md) - Non-interactive mode documentation
- [SimulatedExecutor](./simulated_executor.md) - Order execution simulation
