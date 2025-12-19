# BacktestRunner

`BacktestRunner` replays historical market data through a strategy, simulates order execution, and collects performance statistics.

```cpp
class BacktestRunner : public ISignalHandler
{
public:
  explicit BacktestRunner(const BacktestConfig& config = {});

  void setStrategy(IStrategy* strategy);
  void setSignalStrategy(SignalStrategy* strategy);
  void addExecutionListener(IOrderExecutionListener* listener);

  BacktestResult run(replay::IMultiSegmentReader& reader);

  void onSignal(const Signal& signal) override;

  SimulatedExecutor& executor() noexcept;
  IClock& clock() noexcept;
  const BacktestConfig& config() const noexcept;
};
```

## Methods

| Method | Description |
|--------|-------------|
| `setStrategy()` | Set strategy (legacy mode, strategy calls executor directly) |
| `setSignalStrategy()` | Set signal-based strategy (recommended) |
| `addExecutionListener()` | Add order execution event listener |
| `run()` | Run backtest, returns result with statistics |
| `executor()` | Access to simulated executor |
| `clock()` | Access to virtual clock |

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
runner.setSignalStrategy(&strategy);

// 3. Data
replay::ReaderFilter filter;
filter.symbols = {1};
auto reader = replay::createMultiSegmentReader("./data", filter);

// 4. Run
BacktestResult result = runner.run(*reader);
auto stats = result.computeStats();
```

## Notes

* `setSignalStrategy()` automatically connects signal handler to strategy
* Virtual clock advances based on event timestamps from reader
* Strategy receives events in the same order as in real-time
