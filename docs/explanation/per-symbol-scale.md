# Per-symbol fixed-point scale

FLOX represents prices and quantities as fixed-point integers, not floating
point. `Price`, `Quantity`, and `Volume` each store a raw `int64` scaled by a
fixed factor. The default factor is `1e8`, eight decimal places. That is
plenty for centralized-exchange instruments.

## Why one fixed scale is not enough

A single `1e8` scale on an `int64` covers roughly eleven significant digits.
That is comfortable for a BTC price near `60000.12345678`, but it cannot hold
both ends of a DEX token at once: a memecoin priced near `1e-10` falls below
the `1e8` tick, while a supply near `1e12` units pushes the raw value toward
the `int64` ceiling. The constraint is dynamic range, not the literal decimal
count.

Widening the integer to 128 bits would solve the range but break four things:
the on-disk tape format stores raw `int64`, the C ABI marshals raw `int64`,
the lock-free position trackers hold raw values in `std::atomic<int64>` (a
128-bit atomic is not lock-free on most targets), and JavaScript numbers lose
precision above `2^53`. So the integer width stays at 64 bits.

## Scale on the symbol

The scale is a per-symbol value carried in `SymbolInfo` (`priceScale`,
`qtyScale`), defaulting to `1e8`. A token whose range does not fit `1e8`
registers a scale chosen for its own value range: a finer factor such as `1e15`
for a sub-cent price, a coarser one such as `1e2` for a quadrillion-unit
supply. It converts through the scale-aware `toDouble(scale)` and
`fromDouble(value, scale)` overloads.

The raw storage is scale-agnostic: the tape records raw ticks, and the scale is
interpretation that is never written to disk. So an `int64` raw is enough, and
the struct layout, the atomics, the tape width, and the C ABI carry no scale.
A symbol or a tape without an explicit scale reads as `1e8`.

## Catching scale mismatches

Because the scale is a runtime value on the symbol, not part of the type, the
compiler cannot prove that two values share a scale. Mixing a value of one
scale with another would compute a wrong number. FLOX checks scale agreement at
runtime instead of in the type system:

- conversions require an explicit scale, so a value is never interpreted under
  the wrong factor by accident;
- a single check, `FLOX_SCALE_CHECK`, asserts scale agreement on arithmetic in
  debug and CI builds and compiles to nothing in release;
- cross-symbol position aggregation normalizes scales through one chokepoint
  before combining values;
- symbol registration validates the scale (positive, within `int64` headroom)
  and rejects an out-of-range value.

Fixed-point multiply and divide narrow their 128-bit intermediate back to
`int64` through a checked cast. An out-of-range result trips the check in a
debug build and clamps to the boundary otherwise, rather than wrapping into a
wrong value.

## Crossing into default-scale components

Per-symbol scale lives on `SymbolInfo`; the value itself does not carry it. The
engine's compute components — AMM pricing, the quoter, position tracking —
operate in the default `1e8` scale, because their arithmetic uses the
compile-time scale. A `Price` built with a fine per-symbol scale must therefore
be rescaled to the default before it is handed to one of those components,
otherwise it is read at the wrong factor and the result is silently off by the
ratio of the two scales.

`Decimal::rescale(fromScale, toScale)` is that explicit bridge: it reinterprets
a value's raw from one scale into another. A DEX price at a `1e15` scale becomes
a default-scale `Price` with `price.rescale(1e15, Price::Scale)` before it
reaches the pricing curve or the position tracker. The conversion is lossy when
the target scale is coarser (a sub-tick value rounds toward zero), which is the
inherent limit of representing a very fine value at a coarser scale. A token
whose range cannot survive the default scale at all needs the larger,
runtime-scale-aware work, not just a rescale.

A value carried into a component at the wrong scale is caught, not silently
miscomputed. In debug and CI builds a `Decimal` carries its scale and the
arithmetic checks it: add and subtract require matching scales, and fixed-point
multiply, divide, and the cross-type operators (`Quantity * Price` and friends)
require the default scale. Mixing a per-symbol-scaled value into one of these
traps with a message naming the operator. In release the scale is not stored
and the checks compile away, so the value stays a plain `int64` with no
overhead. The `sanitizers` CI job builds in debug, so the check runs on every
change.

## Forward compatibility with C++26

The guardrail check is a single macro so it can adopt C++26 contracts when a
toolchain ships them: `FLOX_SCALE_CHECK` expands to `contract_assert` where
contracts are detected and to a plain assertion otherwise. The core stays on
C++23, and an experimental, non-blocking CI lane exercises the contracts path
without gating merges.
