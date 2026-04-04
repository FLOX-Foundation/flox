# MultiModePositionTracker

Position tracking with configurable aggregation: net, per-side (hedging), or grouped (per-order).

## Header

```cpp
#include "flox/position/multi_mode_position_tracker.h"
```

## Aggregation Modes

| Mode | Behavior |
|------|----------|
| `NET` | Single position per symbol. BUY adds, SELL deducts. Automatic flip through zero. |
| `PER_SIDE` | Separate long and short positions. Intent via `reduceOnly`/`closePosition` flags or explicit API. |
| `GROUPED` | Each order creates an individual position. Contingent orders (TP/SL/OCO) grouped by `orderTag`. |

## Explicit Intent API

```cpp
MultiModePositionTracker tracker{1, PositionAggregationMode::PER_SIDE};

tracker.openLong(symbol, price, qty);
tracker.closeLong(symbol, price, qty);
tracker.openShort(symbol, price, qty);
tracker.closeShort(symbol, price, qty);

// With tag for grouped mode:
tracker.openLong(symbol, price, qty, /*tag=*/42);
tracker.closeLong(symbol, price, qty, /*tag=*/42);
```

Works in all modes. In NET mode, `openLong`/`openShort` both aggregate into the net position.

## Snapshot

Atomic read of all position fields in a single lock acquisition:

```cpp
auto snap = tracker.snapshot(symbol);
snap.longQty;       // long side quantity
snap.shortQty;      // short side quantity
snap.longAvgEntry;  // long side average entry price
snap.shortAvgEntry; // short side average entry price
snap.realizedPnl;   // accumulated realized PnL
snap.netQty();      // longQty - shortQty
snap.unrealizedPnl(currentPrice);  // mark-to-market
```

## Position Change Callback

```cpp
tracker.onPositionChange([](SymbolId sym, const auto& snap) {
    log("Position changed: {} net={}", sym, snap.netQty().toDouble());
});
```

Fires after every fill. Called under the lock.

## Exchange Integration

Receives fills via `IOrderExecutionListener`:

```cpp
// Subscribe to OrderExecutionBus
bus.subscribe(&tracker);

// For multiple trackers, use MultiExecutionListener
MultiExecutionListener multi{0};
multi.addListener(&netTracker);
multi.addListener(&perSideTracker);
bus.subscribe(&multi);
```

`onOrderFilled` is called once with the full order quantity for complete fills. `onOrderPartiallyFilled` is called for each partial fill. Do not call both for the same fill.

## Reconciliation

```cpp
PositionReconciler reconciler;
auto mismatches = tracker.reconcile(reconciler, exchangePositions);
```

Holds the lock for the entire operation (atomic across all symbols).

## Reset

```cpp
tracker.reset();  // Clear all positions and PnL
```

## Thread Safety

- All public methods are mutex-protected
- `lockedGroups()` returns a proxy that holds the lock:

```cpp
auto positions = tracker.lockedGroups()->getOpenPositions(symbol);
```

- `groups()` provides raw access without locking (caller must ensure safety)

## Cost Basis

Inherits FIFO/LIFO/AVERAGE from `PositionTracker`. All PnL computed in fixed-point arithmetic (no float conversion).

## See Also

- [PositionTracker](position_tracker.md) - Original net-only tracker
- [AggregatedPositionTracker](../cex/aggregated_position_tracker.md) - Lock-free multi-exchange aggregation
