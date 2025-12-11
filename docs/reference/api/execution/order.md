# Order

`Order` encapsulates all information related to a client-side order, including identifiers, execution parameters, status, and timestamps.

```cpp
struct Order {
  OrderId id{};
  Side side{};
  Price price{};
  Quantity quantity{};
  OrderType type{};
  SymbolId symbol{};

  Quantity filledQuantity{0};

  TimePoint createdAt{};
  std::optional<TimePoint> exchangeTimestamp;
  std::optional<TimePoint> lastUpdated;
  std::optional<TimePoint> expiresAfter;
};
```

## Purpose

* Represent an order's full lifecycle — from submission to final state — including fill progress, exchange timestamps, and status.

## Responsibilities

| Field             | Description                                               |
| ----------------- | --------------------------------------------------------- |
| id                | Globally unique order identifier.                         |
| side              | Buy or sell.                                              |
| price             | Limit price; ignored for market orders.                   |
| quantity          | Total order size in base units.                           |
| type              | `LIMIT`, `MARKET`, or other engine-defined types.         |
| symbol            | Compact numeric symbol reference (`SymbolId`).            |
| filledQuantity    | Accumulated quantity filled so far.                       |
| createdAt         | Local creation timestamp.                                 |
| exchangeTimestamp | When the exchange acknowledged the order (if applicable). |
| lastUpdated       | Timestamp of last known state transition.                 |
| expiresAfter      | Optional expiry deadline (e.g. for IOC/GTC enforcement).  |

## Notes

* Used as the payload in `OrderEvent` messages.
* All timestamps are based on `steady_clock` for monotonic sequencing.
* Immutable once submitted; all updates produce new events and/or replacement orders.
