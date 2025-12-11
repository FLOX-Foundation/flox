# IStrategy

`IStrategy` defines the interface for all trading strategies. It combines market data subscription and subsystem lifecycle control, serving as the main driver of signal generation and order placement.

~~~cpp
class IStrategy : public ISubsystem, public IMarketDataSubscriber {
public:
  virtual ~IStrategy() = default;
};
~~~

## Purpose
* Represent a trading strategy that reacts to market data and drives execution decisions.

## Composition

| Inherits From        | Responsibilities                                         |
|----------------------|----------------------------------------------------------|
| `IMarketDataSubscriber` | Receives `TradeEvent`, `BookUpdateEvent`, `CandleEvent`. |
| `ISubsystem`         | Enables coordinated `start()` / `stop()` during engine run. |

## Notes
* Strategies are typically registered as subscribers to market data buses.
* Lifecycle hooks (`start`, `stop`) are used for setup, parameter resets, or cleanup.
* Strategies are expected to emit orders via `IOrderExecutor`, respecting `IRiskManager` and `IKillSwitch` constraints.
