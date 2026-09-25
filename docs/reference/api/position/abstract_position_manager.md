# IPositionManager

`IPositionManager` maintains real-time position tracking per symbol and reacts to all order execution events. It is a core component for portfolio state management in both live trading and simulation.

```cpp
struct PositionSnapshot {
  Quantity position{};
  std::optional<Price> avgEntryPrice;
};

class IPositionManager : public ISubsystem, public IOrderExecutionListener {
public:
  explicit IPositionManager(SubscriberId id);
  virtual ~IPositionManager() = default;

  virtual Quantity getPosition(SymbolId symbol) const = 0;

  // Average entry price of the open position, or nothing when this manager
  // keeps no cost basis. Defaulted so an existing custom manager compiles.
  virtual std::optional<Price> getAverageEntryPrice(SymbolId symbol) const;

  // Both of the above, from one question. Defaults to the position with no
  // cost basis; override it to answer both from one traversal.
  virtual PositionSnapshot positionSnapshot(SymbolId symbol) const;
};
```

## Purpose

* Track and expose current position sizes for all traded instruments in response to order events.

## Responsibilities

| Method           | Description                                                                               |
| ---------------- | ----------------------------------------------------------------------------------------- |
| `getPosition()`  | Returns net position (long/short/flat) for a given `SymbolId`.                            |
| `getAverageEntryPrice()` | Returns the average entry price of the open position, or `std::nullopt` when the manager keeps no cost basis or the position is flat. |
| `positionSnapshot()` | Returns both values together. This is what the strategy calls on every tick. |
| Execution events | Inherited from `IOrderExecutionListener` — updates position on `FILLED`, `REPLACED`, etc. |

`Strategy` reads `positionSnapshot()` into `SymbolContext::position` and
`SymbolContext::avgEntryPrice` before each handler call, and
`SymbolContext::unrealizedPnl()` is built on the entry price.
A manager that reports nothing leaves the context's unrealized PnL empty, which
surfaces as `NaN` through every binding. Substituting zero for the entry price
instead reports the position's whole notional as profit, which is what the
field did before there was a method to ask.

Both shipped managers implement it. A custom manager inherits the default and
opts in by overriding.

## One question per tick

`refreshPosition()` runs on every trade, every book update and every bar,
while the symbol lock is held. It used to ask `getPosition()` and
`getAverageEntryPrice()` separately, and on both shipped trackers each of
those takes the tracker's mutex — the one the execution thread also wants —
and walks the symbol's lot deque: two acquisitions and two traversals per
market-data tick, for a pair of values that come from the same state under the
same lock. `positionSnapshot()` is that one question, and both trackers answer
it under a single acquisition (`PositionTracker` in a single pass over the
lots).

Its default deliberately returns the position with **no** cost basis rather
than calling the two getters, which would put the second acquisition straight
back. A manager that keeps a cost basis overrides `positionSnapshot()`; one
that does not keeps reporting exactly what the default `getAverageEntryPrice()`
already said — nothing.

Not named `snapshot()`: `MultiModePositionTracker::snapshot()` already exists
and means something else (its per-side long/short breakdown).

## Notes

* Acts as a persistent state store for strategies, risk systems, and reporting.
* Must be registered with `OrderExecutionBus` to receive fill and cancel notifications.
* Can optionally implement position limits or exposure constraints internally.

## Implementations

| Class | Use Case |
|-------|----------|
| [PositionTracker](position_tracker.md) | Net position with FIFO/LIFO/AVERAGE cost basis |
| [MultiModePositionTracker](multi_mode_position_tracker.md) | Net, per-side (hedging), or grouped (per-order) aggregation |
| [AggregatedPositionTracker](../cex/aggregated_position_tracker.md) | Lock-free multi-exchange aggregation |

## See Also

- [PositionTracker](position_tracker.md) - Net-only, lot-based
- [MultiModePositionTracker](multi_mode_position_tracker.md) - Multi-mode with reconciliation
- [PositionReconciler](position_reconciler.md) - Exchange position reconciliation
