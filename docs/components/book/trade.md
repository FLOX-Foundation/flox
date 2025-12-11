# Trade

`Trade` represents a single executed transaction between a buyer and seller on a specific instrument, including price, quantity, taker side, and instrument class.

```cpp
struct Trade
{
  SymbolId symbol{};
  InstrumentType instrument = InstrumentType::Spot;
  Price price{};
  Quantity quantity{};
  bool isBuy{false};
  UnixNanos exchangeTsNs{0};
};
```

## Purpose

* Convey executed market activity in a normalized, low-allocation format for downstream components.

## Fields

| Field            | Description                                                             |
| ---------------- | ----------------------------------------------------------------------- |
| **symbol**       | Unique `SymbolId` of the traded instrument.                             |
| **instrument**   | Instrument class: `Spot`, `Future`, or `Option`.                        |
| **price**        | Execution price.                                                        |
| **quantity**     | Executed size (base units).                                             |
| **isBuy**        | `true` if the taker was the buyer; `false` if the taker was the seller. |
| **exchangeTsNs** | Exchange timestamp in nanoseconds since Unix epoch.                     |

## Notes

* Emitted via `TradeEvent` through `TradeBus`.
* Serves as input for candle aggregation, PnL tracking, flow analysis, and latency metrics.
* `instrument` allows immediate filtering without a registry lookup in hot paths.
* Uses `UnixNanos` (int64_t nanoseconds) for precise exchange timestamps.
