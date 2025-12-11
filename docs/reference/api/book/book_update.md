# BookUpdate

`BookUpdate` is a zero-allocation container for transmitting order-book snapshots or deltas. It supports multiple instrument classes (spot, futures, options) and includes optional option metadata.

```cpp
struct BookUpdate {
  SymbolId                    symbol{};                          // instrument identifier
  InstrumentType              instrument = InstrumentType::Spot; // Spot | Future | Inverse | Option
  BookUpdateType              type{};                            // SNAPSHOT | DELTA
  std::pmr::vector<BookLevel> bids;                              // depth on bid side
  std::pmr::vector<BookLevel> asks;                              // depth on ask side

  UnixNanos exchangeTsNs{0};                                     // exchange timestamp (ns)
  UnixNanos systemTsNs{0};                                       // local receive timestamp (ns)

  // Option-specific fields
  std::optional<Price>      strike;                              // strike price
  std::optional<TimePoint>  expiry;                              // option expiry
  std::optional<OptionType> optionType;                          // Call | Put

  explicit BookUpdate(std::pmr::memory_resource* res)
      : bids(res), asks(res) {}
};
```

## Supporting Types

### `BookUpdateType`

```cpp
enum class BookUpdateType {
  SNAPSHOT,  // Full book replacement
  DELTA      // Incremental update
};
```

### `BookLevel`

```cpp
struct BookLevel {
  Price    price{};
  Quantity quantity{};

  BookLevel() = default;
  BookLevel(Price p, Quantity q) : price(p), quantity(q) {}
};
```

## Purpose

* Provide a **normalized, memory-efficient** representation of an order-book update (full snapshot or incremental delta).
* Embed the **instrument class** so downstream components can branch without a registry lookup.

## Fields

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `SymbolId` | Unique identifier of the instrument. |
| `instrument` | `InstrumentType` | `Spot`, `Future`, `Inverse`, or `Option`. |
| `type` | `BookUpdateType` | `SNAPSHOT` (full overwrite) or `DELTA` (incremental change). |
| `bids` | `std::pmr::vector<BookLevel>` | Bid side depth levels. |
| `asks` | `std::pmr::vector<BookLevel>` | Ask side depth levels. |
| `exchangeTsNs` | `UnixNanos` | Exchange timestamp in nanoseconds. |
| `systemTsNs` | `UnixNanos` | Local system receive timestamp in nanoseconds. |
| `strike` | `std::optional<Price>` | Strike price (options only). |
| `expiry` | `std::optional<TimePoint>` | Expiry date/time (options only). |
| `optionType` | `std::optional<OptionType>` | `CALL` or `PUT` (options only). |

## Notes

* When `type == SNAPSHOT`, consumers **must** fully replace their local book for `symbol`.
* `bids` and `asks` use PMR vectors backed by a pool allocator, avoiding runtime allocations.
* Downstream filters can quickly ignore instruments by checking `instrument` without a `SymbolRegistry` lookup.
* **Option fields are optional:** they are populated only when `instrument == InstrumentType::Option`.
* Dual timestamps (`exchangeTsNs` and `systemTsNs`) enable accurate latency measurement.

## See Also

* [BookUpdateEvent](events/book_update_event.md) — Event wrapper for BookUpdate
* [BookUpdateBus](bus/book_update_bus.md) — Event bus for book updates
* [NLevelOrderBook](nlevel_order_book.md) — Order book implementation
* [Common Types](../common.md) — `Price`, `InstrumentType`, `OptionType` definitions
* [Pool](../util/memory/pool.md) — Object pool for BookUpdate allocation
