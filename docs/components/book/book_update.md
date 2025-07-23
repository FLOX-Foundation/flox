# BookUpdate

`BookUpdate` is a zero-allocation container for transmitting order-book snapshots or deltas.  
It now supports multiple instrument classes (spot, futures, options) and includes optional option metadata.

```cpp
struct BookUpdate
{
    SymbolId              symbol{};                          // instrument identifier
    InstrumentType        instrument = InstrumentType::Spot; // Spot | Future | Option
    BookUpdateType        type{};                            // SNAPSHOT | DELTA
    std::pmr::vector<BookLevel> bids;                        // depth on bid side
    std::pmr::vector<BookLevel> asks;                        // depth on ask side
    TimePoint             timestamp{};                       // receive-time

    // — Option-specific fields —
    std::optional<Price>       strike;                       // strike price
    std::optional<TimePoint>   expiry;                       // option expiry
    std::optional<OptionType>  optionType;                   // Call | Put

    explicit BookUpdate(std::pmr::memory_resource* res)
        : bids(res), asks(res) {}
};
```

## Purpose

* Provide a **normalized, memory-efficient** representation of an order-book update (full snapshot or incremental delta).  
* Embed the **instrument class** so downstream components can branch without a registry lookup.

## Responsibilities

| Field / Aspect | Description |
|----------------|-------------|
| **symbol**     | Unique `SymbolId` of the instrument. |
| **instrument** | `Spot`, `Future`, or `Option`. |
| **type**       | `SNAPSHOT` (full overwrite) or `DELTA` (incremental change). |
| **bids / asks**| Depth updates stored in PMR vectors (`BookLevel`). |
| **timestamp**  | Local receive time, used for sequencing and latency metrics. |
| **strike**     | Strike price — *only* for option updates. |
| **expiry**     | Expiry date/time — *only* for option updates. |
| **optionType** | `Call` or `Put` — *only* for option updates. |

## Notes

* When `type == SNAPSHOT`, consumers **must** fully replace their local book for `symbol`.  
* `bids` and `asks` are typically reserved to capacity in an object pool, avoiding runtime allocations.  
* Downstream filters can quickly ignore instruments by checking `instrument` without a `SymbolRegistry` lookup.  
* **Option fields are optional:** they are populated only when `instrument == InstrumentType::Option`.
