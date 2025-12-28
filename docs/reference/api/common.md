# Common Types

This header defines core types and enums used throughout the FLOX engine, including identifiers, numeric types (price, quantity), and domain-specific enums.

## Enums

### `InstrumentType`

Represents the type of financial instrument.

```cpp
enum class InstrumentType {
  Spot,     // Spot market
  Future,   // Futures contract
  Inverse,  // Inverse perpetual
  Option    // Options contract
};
```

### `OptionType`

Represents the type of option contract.

```cpp
enum class OptionType {
  CALL,  // Call option
  PUT    // Put option
};
```

### `OrderType`

Represents the execution style of an order.

```cpp
enum class OrderType : uint8_t {
  LIMIT = 0,              // Limit order
  MARKET = 1,             // Market order
  STOP_MARKET = 2,        // Stop market (triggers market order when price crosses trigger)
  STOP_LIMIT = 3,         // Stop limit (triggers limit order when price crosses trigger)
  TAKE_PROFIT_MARKET = 4, // Take profit market (triggers market order at profit target)
  TAKE_PROFIT_LIMIT = 5,  // Take profit limit (triggers limit order at profit target)
  TRAILING_STOP = 6,      // Trailing stop (trigger follows price movement)
  ICEBERG = 7,            // Iceberg (shows only partial quantity)
};
```

| Type | Description |
|------|-------------|
| `LIMIT` | Standard limit order, placed in the order book |
| `MARKET` | Executes immediately at best available price |
| `STOP_MARKET` | Market order triggered when price crosses trigger price |
| `STOP_LIMIT` | Limit order triggered when price crosses trigger price |
| `TAKE_PROFIT_MARKET` | Market order triggered at profit target |
| `TAKE_PROFIT_LIMIT` | Limit order triggered at profit target |
| `TRAILING_STOP` | Stop that follows favorable price movement |
| `ICEBERG` | Shows only visible portion of total quantity |

### `TimeInForce`

Specifies how long an order remains active.

```cpp
enum class TimeInForce : uint8_t {
  GTC = 0,       // Good Till Cancel (default)
  IOC = 1,       // Immediate Or Cancel
  FOK = 2,       // Fill Or Kill
  GTD = 3,       // Good Till Date
  POST_ONLY = 4, // Maker only (rejected if would take)
};
```

| Policy | Description |
|--------|-------------|
| `GTC` | Remains active until filled or explicitly canceled |
| `IOC` | Fills immediately (partially or fully), cancels remainder |
| `FOK` | Fills completely or rejects entirely |
| `GTD` | Active until specified expiration time |
| `POST_ONLY` | Rejected if would immediately match (maker only) |

### `Side`

Represents the direction of an order.

```cpp
enum class Side {
  BUY,   // Buy side
  SELL   // Sell side
};
```

## Identifiers

| Type | Underlying | Description |
|------|------------|-------------|
| `SymbolId` | `uint32_t` | Unique identifier for a symbol. |
| `OrderId` | `uint64_t` | Unique identifier for an order. |

## Fixed-Point Types

Built on top of the `Decimal` template for safe, precise arithmetic.

```cpp
// Tag types for strong typing
struct PriceTag {};
struct QuantityTag {};
struct VolumeTag {};

// tick = 0.000001 for everything
using Price = Decimal<PriceTag, 100'000'000, 1>;
using Quantity = Decimal<QuantityTag, 100'000'000, 1>;
using Volume = Decimal<VolumeTag, 100'000'000, 1>;
```

| Type | Scale | Description |
|------|-------|-------------|
| `Price` | 1e-6 | Decimal representation of price. |
| `Quantity` | 1e-6 | Decimal quantity (e.g. number of contracts). |
| `Volume` | 1e-6 | Price × Quantity, used in bars etc. |

All three types use `Decimal<Tag, 1'000'000, 1>` internally, ensuring:

* High precision (8 decimal places)
* Strong typing (tags prevent mixing price and quantity)
* Tick-aligned operations and rounding support

## Notes

* These types are used pervasively across all order-related and market data structures.
* Prevents accidental unit mismatches (e.g., adding price and quantity).
* Tick size granularity is currently fixed to `1`.
* `InstrumentType::Inverse` is used for inverse perpetual contracts where PnL is in base currency.

## See Also

* [Decimal](util/base/decimal.md) — Fixed-point decimal implementation
* [Trade](book/trade.md) — Uses `Price`, `Quantity`, `InstrumentType`
* [BookUpdate](book/book_update.md) — Uses `Price`, `InstrumentType`, `OptionType`
* [Order](execution/order.md) — Uses `Price`, `Quantity`, `Side`, `OrderType`
