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

## Notes

* Scale is enforced at compile time — `Decimal<PriceTag, 1000>` is a distinct type from `Decimal<QuantityTag, 1000>`.
* No virtual overhead, heap allocation, or runtime type checks.
* Supports tick-based alignment and arithmetic directly without conversions.
* Used throughout Flox for price, quantity, and other numeric domains.
