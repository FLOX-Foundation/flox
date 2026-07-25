# IStrategy

`IStrategy` defines the interface for all trading strategies. It combines market data subscription and subsystem lifecycle control.

```cpp
class IStrategy : public ISubsystem, public IMarketDataSubscriber {
public:
  virtual ~IStrategy() = default;

  virtual void setSignalHandler(ISignalHandler*) {}
  virtual void setPositionManager(IPositionManager*) {}

  // Forwarded by the runner each time the executor publishes an OrderEvent for
  // an order this strategy emitted. Default no-op; the concrete Strategy
  // dispatches to onSymbolFill / onSymbolOrderUpdate based on status.
  virtual void onOrderEvent(const OrderEvent&) {}
};
```

## Own Methods

| Method | Description |
|--------|-------------|
| `setSignalHandler(ISignalHandler*)` | Install the sink that converts emitted `Signal`s into orders. `BacktestRunner::setStrategy` calls this. Default no-op. |
| `setPositionManager(IPositionManager*)` | Install the position source the strategy reads from. Default no-op. |
| `onOrderEvent(const OrderEvent&)` | Every executor event for an order this strategy emitted. Default no-op, so an implementer need not override it; `Strategy` overrides it and dispatches by status. |

## Purpose

* Define the contract for trading strategies that react to market data and emit order signals.

## Composition

| Inherits From        | Responsibilities                                         |
|----------------------|----------------------------------------------------------|
| `IMarketDataSubscriber` | Receives `TradeEvent`, `BookUpdateEvent`, `BarEvent`. |
| `ISubsystem`         | Enables coordinated `start()` / `stop()` during engine run. |

## Implementation

Use the `Strategy` base class which provides:

- Per-symbol context management (`SymbolContext`)
- Automatic order book maintenance
- Event routing to symbol-specific handlers
- Signal emission helpers

See [Strategy](strategy.md) for the recommended implementation pattern.

## See Also

- [Strategy](strategy.md) - Unified strategy base class
- [SymbolContext](symbol_context.md) - Per-symbol state
