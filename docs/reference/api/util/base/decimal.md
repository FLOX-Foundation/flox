# Decimal

`Decimal` is a fixed-point arithmetic wrapper designed for performance-critical environments such as HFT. It provides type-safe arithmetic on scaled integers with configurable tick precision and compile-time guarantees.

```cpp
template <typename Tag, int Scale, int64_t TickSize = 1>
class Decimal {
  // ...
};
```

## Purpose

* Avoid floating-point rounding errors by using integer math with fixed scaling.
* Provide clean, zero-cost abstractions for price/quantity units with compile-time type safety.

## Parameters

| Template Param | Description                                                           |
| -------------- | --------------------------------------------------------------------- |
| `Tag`          | Phantom type used to disambiguate unit domains (e.g. `Price`, `Qty`). |
| `Scale`        | Number of sub-units per whole unit (e.g. 1000 = 3 decimal places).    |
| `TickSize`     | Granularity for tick-based rounding.                                  |


## Key Features

| Function                    | Description                                                                  |
| --------------------------- | ---------------------------------------------------------------------------- |
| `fromDouble(double)`        | Converts a floating-point value to scaled integer with rounding.             |
| `toDouble()`                | Converts internal `_raw` value to `double` for logging/debugging.            |
| `raw()`                     | Returns raw internal `int64_t` value.                                        |
| `roundToTick()`             | Rounds to the nearest multiple of `TickSize`.                                |
| `isZero()`                  | True if `_raw == 0`.                                                         |
| Arithmetic / Comparison Ops | Full suite of `+`, `-`, `*`, `/`, `==`, `<`, `<=`, etc. on same-type values. |

## Division by zero

An integer divide by zero behaves differently on every target. x86 raises a
hardware exception, AArch64 returns an unspecified value and does not trap, and
the 128-bit software path returns whatever the runtime helper left behind.
Dividing a filled volume by a zero filled quantity used to land in that last
case: a believable execution price, and a different one depending on how the
build was optimized.

Every fixed-point division checks its divisor first:

* In a checked build (`FLOX_SCALE_CHECKS`, on by default without `NDEBUG`) a
  zero divisor trips the guardrail and the process stops there.
* In an unchecked build the result saturates: `INT64_MAX` for a positive
  numerator, `INT64_MIN` for a negative one, zero for `0 / 0`. Fixed-point
  overflow is already treated this way. The point is a number at the int64
  boundary, which no real price or quantity can be mistaken for.
* `flox::fixedPointDivisionsByZero()` counts how often that happened, so a
  release build has something to read; `flox::resetFixedPointDivisionsByZero()`
  clears the counter. An ordinary division touches neither.

This covers `Decimal / Decimal`, `Decimal / int64_t`, and the
`Volume / Quantity` and `Volume / Price` overloads in `flox/common.h`.

## Notes

* Scale is enforced at compile time — `Decimal<PriceTag, 1000>` is a distinct type from `Decimal<QuantityTag, 1000>`.
* No virtual overhead, heap allocation, or runtime type checks.
* Supports tick-based alignment and arithmetic directly without conversions.
* Used throughout FLOX for price, quantity, and other numeric domains.
