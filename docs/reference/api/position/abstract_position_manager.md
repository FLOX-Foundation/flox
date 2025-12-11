# IPositionManager

`IPositionManager` maintains real-time position tracking per symbol and reacts to all order execution events. It is a core component for portfolio state management in both live trading and simulation.

```cpp
class IPositionManager : public ISubsystem, public IOrderExecutionListener {
public:
  explicit IPositionManager(SubscriberId id);
  virtual ~IPositionManager() = default;

  virtual Quantity getPosition(SymbolId symbol) const = 0;
};
```

## Purpose

* Track and expose current position sizes for all traded instruments in response to order events.

## Responsibilities

| Method           | Description                                                                               |
| ---------------- | ----------------------------------------------------------------------------------------- |
| `getPosition()`  | Returns net position (long/short/flat) for a given `SymbolId`.                            |
| Execution events | Inherited from `IOrderExecutionListener` — updates position on `FILLED`, `REPLACED`, etc. |

## Notes

* Acts as a persistent state store for strategies, risk systems, and reporting.
* Must be registered with `OrderExecutionBus` to receive fill and cancel notifications.
* Can optionally implement position limits or exposure constraints internally.
