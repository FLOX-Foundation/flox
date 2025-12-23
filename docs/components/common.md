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
enum class OrderType
{
  LIMIT,
  MARKET
};
```

### `Side`

Represents the direction of an order.

```cpp
enum class Side
{
  BUY,
  SELL
};
```


## Identifiers

| Type       | Underlying  | Description                     |
| ---------- | ----------- | ------------------------------- |
| `SymbolId` | `uint32_t`  | Unique identifier for a symbol. |
| `OrderId`  | `uint64_t`  | Unique identifier for an order. |


## Fixed-Point Types

Built on top of the `Decimal` template for safe, precise arithmetic.

| Type       | Scale | Description                                  |
| ---------- | ----- | -------------------------------------------- |
| `Price`    | 1e-6  | Decimal representation of price.             |
| `Quantity` | 1e-6  | Decimal quantity (e.g. number of contracts). |
| `Volume`   | 1e-6  | Price × Quantity, used in candle bars etc.   |

All three types use `Decimal<Tag, 1'000'000, 1>` internally, ensuring:

* High precision (8 decimal places)
* Strong typing (tags prevent mixing price and size)
* Tick-aligned operations and rounding support


## Notes

* These types are used pervasively across all order-related and market data structures.
* Prevents accidental unit mismatches (e.g., adding price and quantity).
* Tick size granularity is currently fixed to `1`.
* `InstrumentType::Inverse` is used for inverse perpetual contracts.
