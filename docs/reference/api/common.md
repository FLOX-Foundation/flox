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
enum class OrderType {
  LIMIT,   // Limit order
  MARKET   // Market order
};
```

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
using Price = Decimal<PriceTag, 1'000'000, 1>;
using Quantity = Decimal<QuantityTag, 1'000'000, 1>;
using Volume = Decimal<VolumeTag, 1'000'000, 1>;
```

| Type | Scale | Description |
|------|-------|-------------|
| `Price` | 1e-6 | Decimal representation of price. |
| `Quantity` | 1e-6 | Decimal quantity (e.g. number of contracts). |
| `Volume` | 1e-6 | Price × Quantity, used in candle bars etc. |

All three types use `Decimal<Tag, 1'000'000, 1>` internally, ensuring:

* High precision (6 decimal places)
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
