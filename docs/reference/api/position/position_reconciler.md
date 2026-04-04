# PositionReconciler

Detects and resolves discrepancies between local position state and exchange-reported positions.

## Header

```cpp
#include "flox/position/position_reconciler.h"
```

## Usage

### With MultiModePositionTracker

```cpp
MultiModePositionTracker tracker{1, PositionAggregationMode::NET};

std::vector<ExchangePosition> fromExchange = {
    {BTCUSDT, Quantity::fromDouble(0.5), Price::fromDouble(99000.0)},
    {ETHUSDT, Quantity::fromDouble(2.0), Price::fromDouble(3500.0)},
};

PositionReconciler reconciler;
auto mismatches = tracker.reconcile(reconciler, fromExchange);
```

### With Custom Position Source

```cpp
auto mismatches = reconciler.reconcile(exchangePositions,
    [&](SymbolId sym) -> std::pair<Quantity, Price> {
        return {getMyPosition(sym), getMyAvgEntry(sym)};
    });
```

### Bidirectional

Pass local symbols to detect positions that exist locally but not at the exchange:

```cpp
auto mismatches = reconciler.reconcile(exchangePositions, localPosFn,
    /*localSymbols=*/{BTCUSDT, ETHUSDT, SOLUSDT});
```

### Auto-Apply

```cpp
reconciler.setMismatchHandler([](const PositionMismatch& m) {
    log("Mismatch on {}: local={} exchange={}",
        m.symbol, m.localQuantity.toDouble(), m.exchangeQuantity.toDouble());
    return ReconcileAction::ACCEPT_EXCHANGE;
});

reconciler.reconcileAndApply(exchangePositions, localPosFn,
    [&](SymbolId sym, Quantity qty, Price avgEntry) {
        overrideLocalPosition(sym, qty, avgEntry);
    });
```

## Reconcile Actions

| Action | Effect |
|--------|--------|
| `ACCEPT_EXCHANGE` | Trust exchange, call adjustFn to update local |
| `ACCEPT_LOCAL` | Trust local, ignore exchange |
| `FLAG_ONLY` | Log mismatch, do not adjust |

## See Also

- [MultiModePositionTracker](multi_mode_position_tracker.md)
