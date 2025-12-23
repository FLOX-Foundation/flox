# BacktestRunner

`BacktestRunner` replays historical market data through a strategy, simulates order execution, and collects performance statistics. Supports both batch and interactive modes.

```cpp
class BacktestRunner : public ISignalHandler
{
public:
  using EventCallback = std::function<void(const replay::ReplayEvent&, const BacktestState&)>;
  using PauseCallback = std::function<void(const BacktestState&)>;

  explicit BacktestRunner(const BacktestConfig& config = {});

  // Strategy setup
  void setStrategy(IStrategy* strategy);
  void setStrategy(Strategy* strategy);
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

  // Callbacks (interactive mode)
  void setEventCallback(EventCallback cb);
  void setPauseCallback(PauseCallback cb);

  // Results
  BacktestResult result() const;

  // ISignalHandler
  void onSignal(const Signal& signal) override;

  // Access internals
  SimulatedExecutor& executor() noexcept;
  IClock& clock() noexcept;
  const BacktestConfig& config() const noexcept;
};
```

## Two Modes

### Non-Interactive Mode

Synchronous execution from start to end:

```cpp
BacktestRunner runner(config);
runner.setStrategy(&strategy);

// Blocks until complete
BacktestResult result = runner.run(*reader);
```

### Interactive Mode

Async execution with pause/step control. See [Interactive Backtest Mode](./interactive_runner.md) for full documentation.

```cpp
BacktestRunner runner(config);
runner.setStrategy(&strategy);

// Start in background (begins paused)
std::thread t([&]() { runner.start(*reader); });

// Control execution
runner.step();    // One event
runner.resume();  // Run until breakpoint/end
runner.pause();   // Pause execution

t.join();
```

## Strategy Setup

Two overloads for different strategy types:

```cpp
// For IStrategy* (legacy, strategy calls executor directly)
void setStrategy(IStrategy* strategy);

// For Strategy* (signal-based, recommended)
void setStrategy(Strategy* strategy);
```

The signal-based approach connects the strategy's signal handler automatically.

## Data Flow

```
replay::ReplayEvent
        │
        ▼
  BacktestRunner
        │
        ├──► SimulatedExecutor.onTrade() / onBookUpdate()
        │
        └──► Strategy.onTrade() / onBookUpdate()
                    │
                    ▼
              emitMarketBuy() / emitMarketSell()
                    │
                    ▼
           BacktestRunner.onSignal()
                    │
                    ▼
           SimulatedExecutor.submitOrder()
                    │
                    ▼
                  Fill
                    │
                    ▼
             BacktestResult
```

## Usage

```cpp
// 1. Config
BacktestConfig config;
config.initialCapital = 10000.0;
config.feeRate = 0.0004;

BacktestRunner runner(config);

// 2. Strategy
MyStrategy strategy(/*params*/);
runner.setStrategy(&strategy);

// 3. Execution listeners (optional)
runner.addExecutionListener(&positionTracker);

// 4. Data
replay::ReaderFilter filter;
filter.symbols = {1};
auto reader = replay::createMultiSegmentReader("./data", filter);

// 5. Run
BacktestResult result = runner.run(*reader);
auto stats = result.computeStats();

std::cout << "Return: " << stats.returnPct << "%\n";
std::cout << "Sharpe: " << stats.sharpeRatio << "\n";
```

## Notes

- Virtual clock advances based on event timestamps from reader
- Strategy receives events in the same order as in real-time
- Signals are converted to orders and submitted to SimulatedExecutor
- All fills are recorded in BacktestResult

## See Also

- [Interactive Backtest Mode](./interactive_runner.md) — Pause, step, breakpoints
- [SimulatedExecutor](./simulated_executor.md) — Order execution simulation
- [BacktestResult](./backtest_result.md) — Performance statistics
