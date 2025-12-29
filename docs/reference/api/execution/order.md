# Order

`Order` encapsulates all information related to a client-side order, including identifiers, execution parameters, status, timestamps, and advanced order features.

```cpp
struct ExecutionFlags
{
  uint8_t reduceOnly : 1 = 0;
  uint8_t closePosition : 1 = 0;
  uint8_t postOnly : 1 = 0;
  uint8_t _reserved : 5 = 0;
};

struct Order
{
  OrderId id{};
  Side side{};
  Price price{};
  Quantity quantity{};
  OrderType type{};
  SymbolId symbol{};

  Quantity filledQuantity{0};

  TimePoint createdAt{};
  std::optional<TimePoint> lastUpdated{};
  std::optional<TimePoint> expiresAfter{};
  std::optional<TimePoint> exchangeTimestamp{};

  // Advanced order fields
  TimeInForce timeInForce{TimeInForce::GTC};
  ExecutionFlags flags{};
  Price triggerPrice{};             // for stop/TP orders
  Price trailingOffset{};           // for trailing stop (fixed)
  int32_t trailingCallbackRate{0};  // for trailing stop (bps, 100 = 1%)

  // Metadata
  uint64_t clientOrderId{0};
  uint16_t strategyId{0};
  uint16_t orderTag{0};  // for OCO grouping

  // Iceberg
  Quantity visibleQuantity{};  // visible size (0 = full)
};
```

## Purpose

* Represent an order's full lifecycle — from submission to final state — including fill progress, exchange timestamps, and status.

## Core Fields

| Field             | Description                                               |
| ----------------- | --------------------------------------------------------- |
| id                | Globally unique order identifier.                         |
| side              | Buy or sell.                                              |
| price             | Limit price; ignored for market orders.                   |
| quantity          | Total order size in base units.                           |
| type              | `LIMIT`, `MARKET`, `STOP_MARKET`, `STOP_LIMIT`, etc.      |
| symbol            | Compact numeric symbol reference (`SymbolId`).            |
| filledQuantity    | Accumulated quantity filled so far.                       |

## Timestamps

| Field             | Description                                               |
| ----------------- | --------------------------------------------------------- |
| createdAt         | Local creation timestamp.                                 |
| lastUpdated       | Timestamp of last known state transition.                 |
| expiresAfter      | Optional expiry deadline (e.g. for IOC/GTD enforcement).  |
| exchangeTimestamp | When the exchange acknowledged the order (if applicable). |

## Advanced Order Fields

| Field               | Description                                             |
| ------------------- | ------------------------------------------------------- |
| timeInForce         | `GTC`, `IOC`, `FOK`, `GTD` (default: GTC).              |
| flags               | Execution flags (see below).                            |
| triggerPrice        | Trigger price for stop/take-profit orders.              |
| trailingOffset      | Fixed offset for trailing stop orders.                  |
| trailingCallbackRate| Trailing stop callback rate in bps (100 = 1%).          |

## ExecutionFlags

| Flag          | Description                                   |
| ------------- | --------------------------------------------- |
| reduceOnly    | Order can only reduce position, not increase. |
| closePosition | Order should close entire position.           |
| postOnly      | Order must be maker; rejects if would take.   |

## Metadata

| Field         | Description                                     |
| ------------- | ----------------------------------------------- |
| clientOrderId | User-defined ID for correlation.                |
| strategyId    | ID of the strategy that created the order.      |
| orderTag      | Tag for order grouping (e.g., OCO pairs).       |

## Iceberg Orders

| Field           | Description                                   |
| --------------- | --------------------------------------------- |
| visibleQuantity | Visible size in order book (0 = full order).  |

## Notes

* Used as the payload in `OrderEvent` messages.
* All timestamps are based on `steady_clock` for monotonic sequencing.
* Immutable once submitted; all updates produce new events and/or replacement orders.
* Advanced fields support conditional orders (stop-loss, take-profit, trailing stop).
* `orderTag` enables OCO (one-cancels-other) order grouping.
