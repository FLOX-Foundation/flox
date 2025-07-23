# Trade

`Trade` represents a single executed transaction between a buyer and seller on a specific instrument, including price, quantity, taker side, and instrument class.

```cpp
struct Trade {
    SymbolId       symbol{};                             // instrument identifier
    InstrumentType instrument = InstrumentType::Spot;    // Spot | Future | Option
    Price          price{};                              // execution price
    Quantity       quantity{};                           // size in base units
    bool           isBuy{false};                         // taker side (true = buy)
    TimePoint      timestamp{};                          // execution time
};
```

## Purpose

* Convey executed market activity in a normalized, low-allocation format for downstream components.

## Responsibilities

| Field          | Description                                                             |
| -------------- | ----------------------------------------------------------------------- |
| **symbol**     | Unique `SymbolId` of the traded instrument.                             |
| **instrument** | Instrument class: `Spot`, `Future`, or `Option`.                        |
| **price**      | Execution price.                                                        |
| **quantity**   | Executed size (base units).                                             |
| **isBuy**      | `true` if the taker was the buyer; `false` if the taker was the seller. |
| **timestamp**  | Event or wall-clock time of execution (`steady_clock` basis).           |

## Notes

* Emitted via `TradeEvent` through `TradeBus`.
* Serves as input for candle aggregation, PnL tracking, flow analysis, and latency metrics.
* `instrument` allows immediate filtering without a registry lookup in hot paths.
