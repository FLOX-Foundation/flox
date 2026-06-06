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

## The approach: scale moves to the symbol

Instead of widening the integer, the scale becomes a per-symbol value carried
in `SymbolInfo` (`priceScale`, `qtyScale`), defaulting to `1e8`. A token whose
range does not fit `1e8` registers a scale chosen for its own value range: a
finer factor such as `1e15` for a sub-cent price, a coarser one such as `1e2`
for a quadrillion-unit supply. It then converts through the scale-aware
`toDouble(scale)` and `fromDouble(value, scale)` overloads.

Because the raw storage is already scale-agnostic (the tape records raw ticks;
the scale is interpretation, never written to disk), keeping `int64` leaves the
struct layout, the atomics, the tape width, and the C ABI untouched. Existing
symbols and previously recorded data default to `1e8` and behave identically.

## Recovering the lost guarantee

When the scale lived in the type, the compiler guaranteed every `Price` shared
one scale. Moving the scale to runtime removes that compile-time check, so a
raw value of one scale could be mixed with another and produce a silent error.

FLOX recovers the guarantee with guardrails rather than the type system:

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

## Forward compatibility with C++26

The guardrail check is a single macro so it can adopt C++26 contracts when a
toolchain ships them: `FLOX_SCALE_CHECK` expands to `contract_assert` where
contracts are detected and to a plain assertion otherwise. The core stays on
C++23, and an experimental, non-blocking CI lane exercises the contracts path
without gating merges.
