# Trade

`Trade` represents a single executed transaction between a buyer and seller on a specific instrument, including price, quantity, taker side, and exchange timestamp.

```cpp
struct Trade {
  SymbolId       symbol{};                             // instrument identifier
  InstrumentType instrument = InstrumentType::Spot;    // Spot | Future | Inverse | Option
  Price          price{};                              // execution price
  Quantity       quantity{};                           // size in base units
  bool           isBuy{false};                         // taker side (true = buy)
  UnixNanos      exchangeTsNs{0};                      // exchange timestamp in nanoseconds
};
```

## Purpose

* Convey executed market activity in a normalized, low-allocation format for downstream components.

## Fields

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `SymbolId` | Unique identifier of the traded instrument. |
| `instrument` | `InstrumentType` | Instrument class: `Spot`, `Future`, `Inverse`, or `Option`. |
| `price` | `Price` | Execution price (fixed-point decimal). |
| `quantity` | `Quantity` | Executed size in base units (fixed-point decimal). |
| `isBuy` | `bool` | `true` if the taker was the buyer; `false` if seller. |
| `exchangeTsNs` | `UnixNanos` | Exchange timestamp in nanoseconds since Unix epoch. |

## Notes

* Emitted via `TradeEvent` through `TradeBus`.
* Serves as input for bar aggregation, PnL tracking, flow analysis, and latency metrics.
* `instrument` allows immediate filtering without a registry lookup in hot paths.
* Uses `UnixNanos` (nanosecond precision) for accurate timestamp handling across exchanges.

## See Also

* [TradeEvent](events/trade_event.md) — Event wrapper for Trade
* [TradeBus](bus/trade_bus.md) — Event bus for trade events
* [Common Types](../common.md) — `Price`, `Quantity`, `InstrumentType` definitions
