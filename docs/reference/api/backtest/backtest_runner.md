# BacktestRunner

Replay-driven backtest executor with optional interactive debugging.

## Overview

`BacktestRunner` replays historical market data through a strategy, simulating order execution and tracking results. Supports both batch mode (run to completion) and interactive mode (step-through, breakpoints).

## Header

```cpp
#include "flox/backtest/backtest_runner.h"
```

## BacktestMode

```cpp
enum class BacktestMode {
  Run,       // Run continuously
  Step,      // Execute one event
  StepTrade  // Execute until next trade
};
```

## Breakpoint

```cpp
struct Breakpoint {
  enum class Type {
    Time,        // Break at timestamp
    EventCount,  // Break after N events
    TradeCount,  // Break after N trades
    Signal,      // Break on strategy signal
    Custom       // Custom predicate
  };

  Type type;
  UnixNanos timestampNs;
  uint64_t count;
  std::function<bool(const replay::ReplayEvent&)> predicate;

  // Builders
  static Breakpoint atTime(UnixNanos ts);
  static Breakpoint afterEvents(uint64_t n);
  static Breakpoint afterTrades(uint64_t n);
  static Breakpoint onSignal();
  static Breakpoint when(std::function<bool(const replay::ReplayEvent&)> pred);
};
```

## BacktestState

```cpp
struct BacktestState {
  UnixNanos currentTimeNs;
  uint64_t eventCount;
  uint64_t tradeCount;
  uint64_t bookUpdateCount;
  uint64_t signalCount;
  bool isRunning;
  bool isPaused;
  bool isFinished;
  std::optional<replay::EventType> lastEventType;
};
```

## Class Definition

```cpp
class BacktestRunner : public ISignalHandler {
public:
  using EventCallback = std::function<void(const replay::ReplayEvent&, const BacktestState&)>;
  using PauseCallback = std::function<void(const BacktestState&)>;

  explicit BacktestRunner(const BacktestConfig& config = {});

  // Strategy setup
  void setStrategy(IStrategy* strategy);
  void addMarketDataSubscriber(IMarketDataSubscriber* subscriber);
  void addExecutionListener(IOrderExecutionListener* listener);

  // Non-interactive mode
  BacktestResult run(replay::IMultiSegmentReader& reader);

  // Interactive mode
  void start(replay::IMultiSegmentReader& reader);
  void resume();
  void step();
  void stepUntil(BacktestMode mode);
  void pause();
  void stop();

  // Breakpoints
  void addBreakpoint(Breakpoint bp);
  void clearBreakpoints();
  void setBreakOnSignal(bool enable);

  // State inspection
  BacktestState state() const;
  bool isPaused() const;
  bool isFinished() const;

  // Callbacks
  void setEventCallback(EventCallback cb);
  void setPauseCallback(PauseCallback cb);

  // Results
  BacktestResult result() const;
  BacktestResult extractResult();

  // ISignalHandler
  void onSignal(const Signal& signal) override;

  // Access internals
  SimulatedExecutor& executor() noexcept;
  IClock& clock() noexcept;
  const BacktestConfig& config() const noexcept;
};
```

## Methods

### Constructor

```cpp
explicit BacktestRunner(const BacktestConfig& config = {});
```

### Strategy Setup

```cpp
void setStrategy(IStrategy* strategy);
void addMarketDataSubscriber(IMarketDataSubscriber* subscriber);
void addExecutionListener(IOrderExecutionListener* listener);
```

Strategy receives signals from backtest, subscribers get market data, listeners get fill notifications.

### run (Non-Interactive)

```cpp
BacktestResult run(replay::IMultiSegmentReader& reader);
```

Run complete backtest synchronously. Returns results when finished.

### Interactive Mode

```cpp
void start(replay::IMultiSegmentReader& reader);  // Start paused
void resume();        // Continue running
void step();          // Execute one event
void stepUntil(BacktestMode mode);  // Step until condition
void pause();         // Pause execution
void stop();          // Stop completely
```

Interactive mode must be started from a separate thread.

### Breakpoints

```cpp
void addBreakpoint(Breakpoint bp);
void clearBreakpoints();
void setBreakOnSignal(bool enable);
```

### Results

```cpp
BacktestResult result() const;       // Copy of current result
BacktestResult extractResult();      // Move result out
```

## Example: Batch Mode

```cpp
BacktestConfig config;
config.initialCapital = 100000.0;

BacktestRunner runner(config);
runner.setStrategy(&myStrategy);

// Load data
BinaryLogReader reader("market_data.bin");

// Run to completion
auto result = runner.run(reader);
auto stats = result.computeStats();

std::cout << "Trades: " << stats.totalTrades << "\n";
std::cout << "Sharpe: " << stats.sharpeRatio << "\n";
```

## Example: Interactive Mode

```cpp
BacktestRunner runner(config);
runner.setStrategy(&myStrategy);

// Set callbacks
runner.setEventCallback([](const replay::ReplayEvent& e, const BacktestState& s) {
  std::cout << "Event " << s.eventCount << " at " << s.currentTimeNs << "\n";
});

runner.setPauseCallback([](const BacktestState& s) {
  std::cout << "Paused at event " << s.eventCount << "\n";
});

// Add breakpoint
runner.addBreakpoint(Breakpoint::afterTrades(10));

// Start in separate thread
std::thread t([&]() {
  runner.start(reader);
});

// Control from main thread
std::this_thread::sleep_for(std::chrono::seconds(1));
runner.step();  // Execute one event
runner.resume();  // Continue

t.join();
auto result = runner.extractResult();
```

## Example: Breakpoints

```cpp
// Time-based
runner.addBreakpoint(Breakpoint::atTime(1609459200000000000));

// Event count
runner.addBreakpoint(Breakpoint::afterEvents(1000));

// Trade count
runner.addBreakpoint(Breakpoint::afterTrades(5));

// On any signal
runner.setBreakOnSignal(true);

// Custom condition
runner.addBreakpoint(Breakpoint::when([](const replay::ReplayEvent& e) {
  return e.type == replay::EventType::Trade &&
         e.trade.price.toDouble() > 50000.0;
}));
```

## See Also

- [BacktestResult](./backtest_result.md)
- [How-to: Backtesting](../../../how-to/backtest.md)
