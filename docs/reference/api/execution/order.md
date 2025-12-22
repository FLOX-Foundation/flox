# Order

`Order` encapsulates all information related to a client-side order, including identifiers, execution parameters, status, and timestamps.

```cpp
struct ExecutionFlags {
  uint8_t reduceOnly : 1 = 0;     // Only reduce existing position
  uint8_t closePosition : 1 = 0;  // Close entire position
  uint8_t postOnly : 1 = 0;       // Maker only (reject if would take)
  uint8_t _reserved : 5 = 0;
};

struct Order {
  OrderId id{};
  Side side{};
  Price price{};
  Quantity quantity{};
  OrderType type{};
  SymbolId symbol{};

  Quantity filledQuantity{0};

  // Advanced order fields
  TimeInForce timeInForce{TimeInForce::GTC};
  ExecutionFlags flags{};
  Price triggerPrice{};             // For stop/TP orders
  Price trailingOffset{};           // For trailing stop (fixed offset)
  int32_t trailingCallbackRate{0};  // For trailing stop (bps, 100 = 1%)

  // Metadata
  uint64_t clientOrderId{0};
  uint16_t strategyId{0};
  uint16_t orderTag{0};  // For OCO grouping

  // Iceberg
  Quantity visibleQuantity{};  // Visible size (0 = full)

  TimePoint createdAt{};
  std::optional<TimePoint> exchangeTimestamp;
  std::optional<TimePoint> lastUpdated;
  std::optional<TimePoint> expiresAfter;
};
```

## Purpose

* Represent an order's full lifecycle — from submission to final state — including fill progress, exchange timestamps, and status.

## Core Fields

| Field           | Description                                               |
|-----------------|-----------------------------------------------------------|
| id              | Globally unique order identifier.                         |
| side            | Buy or sell.                                              |
| price           | Limit price; ignored for market orders.                   |
| quantity        | Total order size in base units.                           |
| type            | Order type (see [OrderType](../common.md#ordertype)).     |
| symbol          | Compact numeric symbol reference (`SymbolId`).            |
| filledQuantity  | Accumulated quantity filled so far.                       |

## Advanced Order Fields

| Field                | Description                                                    |
|----------------------|----------------------------------------------------------------|
| timeInForce          | Order duration policy (see [TimeInForce](../common.md#timeinforce)). |
| flags                | Execution flags (reduceOnly, closePosition, postOnly).         |
| triggerPrice         | Price that activates conditional orders (stop, TP).            |
| trailingOffset       | Fixed price offset for trailing stop.                          |
| trailingCallbackRate | Percentage offset for trailing stop (basis points, 100 = 1%).  |

## Metadata Fields

| Field          | Description                                               |
|----------------|-----------------------------------------------------------|
| clientOrderId  | User-defined order ID for tracking.                       |
| strategyId     | Strategy that created this order.                         |
| orderTag       | Tag for grouping related orders (OCO).                    |
| visibleQuantity| Visible size for iceberg orders (0 = show full quantity). |

## Timestamp Fields

| Field             | Description                                               |
|-------------------|-----------------------------------------------------------|
| createdAt         | Local creation timestamp.                                 |
| exchangeTimestamp | When the exchange acknowledged the order (if applicable). |
| lastUpdated       | Timestamp of last known state transition.                 |
| expiresAfter      | Optional expiry deadline (for GTD orders).                |

## ExecutionFlags

Bitfield struct for efficient storage of execution options:

| Flag          | Description                                        |
|---------------|----------------------------------------------------|
| reduceOnly    | Order can only reduce position, not increase it.   |
| closePosition | Order closes entire position.                      |
| postOnly      | Rejected if would immediately match (maker only).  |

## Conditional Order Trigger Logic

### Stop Orders (STOP_MARKET, STOP_LIMIT)

* **SELL stop**: Triggers when price ≤ triggerPrice (price falling)
* **BUY stop**: Triggers when price ≥ triggerPrice (price rising)

### Take Profit Orders (TAKE_PROFIT_MARKET, TAKE_PROFIT_LIMIT)

* **SELL TP**: Triggers when price ≥ triggerPrice (lock profit on long)
* **BUY TP**: Triggers when price ≤ triggerPrice (lock profit on short)

### Trailing Stop

* **SELL trailing**: Trigger follows price up (never down), triggers when price drops to trigger
* **BUY trailing**: Trigger follows price down (never up), triggers when price rises to trigger

## Notes

* Used as the payload in `OrderEvent` messages.
* All timestamps are based on `steady_clock` for monotonic sequencing.
* Immutable once submitted; all updates produce new events and/or replacement orders.
* New fields have defaults.

## See Also

* [OrderType](../common.md#ordertype) — Order type enum
* [TimeInForce](../common.md#timeinforce) — Time-in-force policies
* [OrderEvent](events/order_event.md) — Order lifecycle events
* [ExchangeCapabilities](exchange_capabilities.md) — Feature discovery
