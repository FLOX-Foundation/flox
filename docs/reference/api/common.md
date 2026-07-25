# Common Types

This header defines core types and enums used throughout the FLOX engine, including identifiers, numeric types (price, quantity), and domain-specific enums.


## Enums

### `InstrumentType`

Represents the class of tradable instrument.

```cpp
enum class InstrumentType
{
  Spot,
  Future,
  Inverse,
  Option
};
```

### `OptionType`

Represents option direction (for option instruments only).

```cpp
enum class OptionType
{
  CALL,
  PUT
};
```

### `OrderType`

Represents the execution style of an order.

```cpp
enum class OrderType : uint8_t
{
  LIMIT = 0,
  MARKET = 1,
  STOP_MARKET = 2,
  STOP_LIMIT = 3,
  TAKE_PROFIT_MARKET = 4,
  TAKE_PROFIT_LIMIT = 5,
  TRAILING_STOP = 6,
  ICEBERG = 7,
};
```

| Type | Description |
|------|-------------|
| `LIMIT` | Limit order at specified price |
| `MARKET` | Market order, immediate execution |
| `STOP_MARKET` | Stop-loss that triggers market order |
| `STOP_LIMIT` | Stop-loss that triggers limit order |
| `TAKE_PROFIT_MARKET` | Take-profit that triggers market order |
| `TAKE_PROFIT_LIMIT` | Take-profit that triggers limit order |
| `TRAILING_STOP` | Trailing stop order |
| `ICEBERG` | Iceberg order with hidden quantity |

### `TimeInForce`

Time-in-force policies for orders.

```cpp
enum class TimeInForce : uint8_t
{
  GTC = 0,        // Good Till Cancel (default)
  IOC = 1,        // Immediate Or Cancel
  FOK = 2,        // Fill Or Kill
  GTD = 3,        // Good Till Date
  POST_ONLY = 4,  // Maker only
};
```

| Policy | Description |
|--------|-------------|
| `GTC` | Order remains until canceled |
| `IOC` | Fill immediately or cancel unfilled portion |
| `FOK` | Fill entire order immediately or cancel |
| `GTD` | Order expires at specified date/time |
| `POST_ONLY` | Order must be maker (rejected if would take) |

### `Side`

Represents the direction of an order.

```cpp
enum class Side
{
  BUY,
  SELL
};
```

### `VenueType`

Type of trading venue.

```cpp
enum class VenueType : uint8_t
{
  CentralizedExchange,
  AmmDex,
  HybridDex
};
```

### `SettlementType`

How a contract settles at expiry. Defaults to `Cash` on `SymbolInfo`, which preserves spot and perp
behavior.

```cpp
enum class SettlementType
{
  Cash,
  Physical
};
```

### `ExerciseStyle`

When an option may be exercised. Defaults to `European` on `SymbolInfo`.

```cpp
enum class ExerciseStyle
{
  European,
  American
};
```

### `STPMode`

Self-trade prevention. Real venues apply these when a new order from the same account would cross an
existing resting order of the same account. `SimulatedExecutor` matches on `Order::accountId`;
configure it with `setSTPMode` and `setSTPGroupMembership`.

```cpp
enum class STPMode : uint8_t
{
  None = 0,          // self-match allowed (default)
  CancelNewest = 1,  // cancel the incoming order
  CancelOldest = 2,  // cancel the resting order; new one goes through
  CancelBoth = 3,    // cancel both legs
  Decrement = 4,     // cancel smaller side fully; reduce larger by smaller qty
};
```

| Value | Name | Effect |
|-------|------|--------|
| 0 | `None` | Self-match allowed. Default |
| 1 | `CancelNewest` | Cancel the incoming order |
| 2 | `CancelOldest` | Cancel the resting order; the new one goes through |
| 3 | `CancelBoth` | Cancel both legs |
| 4 | `Decrement` | Cancel the smaller side fully; reduce the larger by the smaller quantity |


## Identifiers

| Type         | Underlying   | Description                     |
| ------------ | ------------ | ------------------------------- |
| `SymbolId`   | `uint32_t`   | Unique identifier for a symbol. |
| `OrderId`    | `uint64_t`   | Unique identifier for an order. |
| `ExchangeId` | `uint16_t`   | Unique identifier for an exchange. |

```cpp
static constexpr ExchangeId InvalidExchangeId = 0xFFFF;
```


## Fixed-Point Types

Built on top of the `Decimal` template for safe, precise arithmetic.

| Type       | Scale | Description                                  |
| ---------- | ----- | -------------------------------------------- |
| `Price`    | 1e-8  | Decimal representation of price.             |
| `Quantity` | 1e-8  | Decimal quantity (e.g. number of contracts). |
| `Volume`   | 1e-8  | Price × Quantity, used in bars etc.          |

All three types use `Decimal<Tag, 100'000'000, 1>` internally, ensuring:

* High precision (8 decimal places)
* Strong typing (tags prevent mixing price and size)
* Tick-aligned operations and rounding support

### Arithmetic Operations

```cpp
// Volume = Price × Quantity
Volume notional = price * quantity;
Volume notional = quantity * price;  // Also works

// Price = Volume / Quantity
Price avgPrice = totalVolume / totalQty;

// Quantity = Volume / Price
Quantity qty = notional / price;
```

Uses 128-bit arithmetic internally to prevent overflow on large values.


## Notes

* These types are used pervasively across all order-related and market data structures.
* Prevents accidental unit mismatches (e.g., adding price and quantity).
* `InstrumentType::Inverse` is used for inverse perpetual contracts.
* `ExchangeId` is used in `SymbolRegistry` for multi-exchange scenarios.
