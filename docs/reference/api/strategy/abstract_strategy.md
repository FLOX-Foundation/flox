# IStrategy

`IStrategy` defines the interface for all trading strategies. It combines market data subscription and subsystem lifecycle control.

```cpp
class IStrategy : public ISubsystem, public IMarketDataSubscriber {
public:
  virtual ~IStrategy() = default;
};
```

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
