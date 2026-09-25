/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cassert>
#include <cstdint>
#include <ostream>
#include <string>

#include "flox/util/base/scale_check.h"

namespace flox
{

template <typename Tag, int Scale_, int64_t TickSize_ = 1>
class Decimal
{
 public:
  static constexpr int Scale = Scale_;
  static constexpr int64_t TickSize = TickSize_;

  static_assert(Scale > 0, "Decimal requires Scale > 0 for arithmetic");

  // Which of the two arithmetic paths the operators take. GCC and Clang have
  // __int128 and take the native one; MSVC has no such type and takes the
  // software path in scale_check.h. A build that defines
  // FLOX_FORCE_PORTABLE_INT128=1 takes the software path everywhere, which is
  // how tests/test_decimal_portable is run twice and the MSVC arithmetic is
  // covered on macOS and Linux. mulPath / divPath / rescalePath below also
  // take the choice as a template argument, so one binary can check the two
  // against each other without rebuilding.
  static constexpr bool kPortablePath = (FLOX_HAS_NATIVE_INT128 == 0);

  constexpr Decimal() : _raw(0) {}
  explicit constexpr Decimal(int64_t raw) : _raw(raw) {}

  // The runtime scale this value is expressed in. Equals the compile-time
  // Scale unless built through a scale-aware path (fromDouble(val, scale) /
  // rescale). Only stored when scale checks are on; in release it is the
  // compile-time Scale with no storage and no overhead.
  constexpr int64_t scale() const
  {
#if FLOX_SCALE_CHECKS
    return _scale;
#else
    return Scale;
#endif
  }

  static constexpr Decimal fromDouble(double val)
  {
    if constexpr (Scale > 0)
    {
      return Decimal(narrowDoubleToI64(val >= 0.0 ? val * Scale + 0.5 : val * Scale - 0.5));
    }
    else
    {
      return Decimal(0);
    }
  }
  static constexpr Decimal fromRaw(int64_t raw) { return Decimal(raw); }

  constexpr double toDouble() const { return static_cast<double>(_raw) / Scale; }

  // Scale-aware conversions. The compile-time Scale is the default
  // (1e8) used by CEX symbols; a DEX symbol carries its own scale in
  // SymbolInfo and converts through these explicit-scale overloads so the
  // raw int64 is interpreted correctly for its value range.
  constexpr double toDouble(int64_t scale) const
  {
    return static_cast<double>(_raw) / static_cast<double>(scale);
  }
  static constexpr Decimal fromDouble(double val, int64_t scale)
  {
    return withScale(
        narrowDoubleToI64(val >= 0.0 ? val * static_cast<double>(scale) + 0.5
                                     : val * static_cast<double>(scale) - 0.5),
        scale);
  }

  // Reinterpret this value, whose raw was created at `fromScale`, into this
  // type's compile-time scale read at `toScale`. This is the explicit bridge
  // between a per-symbol scale and the default scale the rest of the engine
  // (AMM pricing, quoter, position tracking) computes in: a DEX price built
  // with a fine per-symbol scale must be rescaled before it is handed to a
  // default-scale component, otherwise it is read at the wrong scale. Lossy
  // when toScale is coarser than fromScale (a sub-tick value rounds toward
  // zero), which is inherent to narrowing the scale.
  //
  // The arithmetic goes through mulDivI64As, which carries the product at
  // full width on every toolchain. The branch this used to take without
  // __int128 computed `_raw * toScale` first: a DEX raw at a 1e15 scale is
  // already 1e17, so the product is 1e25 and the int64 it was written in
  // wrapped before the division ever ran.
  template <bool Portable>
  constexpr Decimal rescalePath(int64_t fromScale, int64_t toScale) const
  {
    return withScale(mulDivI64As<Portable>(_raw, toScale, fromScale), toScale);
  }
  constexpr Decimal rescale(int64_t fromScale, int64_t toScale) const
  {
    return rescalePath<kPortablePath>(fromScale, toScale);
  }

  constexpr int64_t raw() const { return _raw; }

  constexpr Decimal roundToTick() const
  {
    return withScale((_raw / TickSize) * TickSize, scale());
  }

  // Comparisons order by raw only, ignoring scale, so the defaulted
  // comparison does not pick up the debug-only _scale member and "x == 0"
  // works across scales.
  constexpr bool operator==(const Decimal& o) const { return _raw == o._raw; }
  constexpr auto operator<=>(const Decimal& o) const { return _raw <=> o._raw; }

  constexpr bool operator<(const Decimal& other) const { return _raw < other._raw; }
  constexpr bool operator>(const Decimal& other) const { return _raw > other._raw; }
  constexpr bool operator<=(const Decimal& other) const { return _raw <= other._raw; }
  constexpr bool operator>=(const Decimal& other) const { return _raw >= other._raw; }

  // Add / subtract require the same scale (a zero operand is scale-agnostic).
  // The result carries that scale.
  constexpr Decimal operator+(Decimal d) const
  {
    FLOX_SCALE_CHECK(_raw == 0 || d._raw == 0 || scale() == d.scale(),
                     "Decimal::operator+ scale mismatch");
    return withScale(checkedAddI64(_raw, d._raw), _raw != 0 ? scale() : d.scale());
  }
  constexpr Decimal operator-(Decimal d) const
  {
    FLOX_SCALE_CHECK(_raw == 0 || d._raw == 0 || scale() == d.scale(),
                     "Decimal::operator- scale mismatch");
    return withScale(checkedSubI64(_raw, d._raw), _raw != 0 ? scale() : d.scale());
  }

  // Scalar multiply / divide preserve the value's scale. The multiply has no
  // Scale to divide the product back down, so it is the one operator here
  // that can leave the int64 range from two ordinary-looking operands; it
  // saturates like every other overflow in this file rather than wrapping.
  constexpr Decimal operator*(int64_t x) const { return withScale(checkedMulI64(_raw, x), scale()); }
  constexpr Decimal operator/(int64_t x) const
  {
    if (x == 0)
    {
      return withScale(dividedByZeroI64(_raw), scale());
    }
    // INT64_MIN / -1 has no int64 answer and is undefined behavior, not a
    // wrap: it is the one division that overflows. Routed through the checked
    // negation so it saturates like the rest.
    if (x == -1)
    {
      return withScale(checkedSubI64(0, _raw), scale());
    }
    return withScale(_raw / x, scale());
  }

  // Fixed-point multiply / divide bake in the compile-time Scale, so both
  // operands must be at the default scale; rescale a per-symbol value first.
  // The result is at the default scale.
  //
  // Both paths are one call to mulDivI64As, which carries the product at full
  // width. What stood here without __int128 was a hand-rolled split into a
  // quotient term and a remainder term, and the remainder term overflows at
  // any ordinary price: 60000.12345678 * 60000 computes 12345678 * 6e12 =
  // 7.4e19 in an int64. That is the same formula common.h removed from
  // Quantity * Price after a backtest returned -534 where 5000 was expected;
  // it survived here because no developer machine compiles this branch.
  // mulPath<true> is how a test on such a machine reaches it anyway.
  template <bool Portable>
  constexpr Decimal mulPath(const Decimal& other) const
  {
    FLOX_SCALE_CHECK(scale() == Scale && other.scale() == Scale,
                     "Decimal::operator* requires default scale; rescale first");
    return Decimal(mulDivI64As<Portable>(_raw, other._raw, Scale));
  }
  constexpr Decimal operator*(const Decimal& other) const { return mulPath<kPortablePath>(other); }
  // The zero-divisor guard used to read `assert(other.isZero() != 0)`, which
  // asserts the divisor IS zero: it aborted every legal division in a build
  // with assertions on, and in a build with them off it let a division by
  // zero through to the platform. Both directions are covered now -- the
  // check trips in checked builds, and the result is defined in all of them.
  //
  // The branch without __int128 wrote the numerator as `_raw * Scale` in an
  // int64: 6e12 * 1e8 = 6e20 for one BTC price, so every price-by-price
  // division on that toolchain overflowed before dividing.
  template <bool Portable>
  constexpr Decimal divPath(const Decimal& other) const
  {
    FLOX_SCALE_CHECK(scale() == Scale && other.scale() == Scale,
                     "Decimal::operator/ requires default scale; rescale first");
    if (other._raw == 0)
    {
      return Decimal(dividedByZeroI64(_raw));
    }
    return Decimal(mulDivI64As<Portable>(_raw, Scale, other._raw));
  }
  constexpr Decimal operator/(const Decimal& other) const { return divPath<kPortablePath>(other); }

  constexpr friend Decimal operator*(int64_t x, Decimal d)
  {
    return withScale(checkedMulI64(x, d._raw), d.scale());
  }

  // Checked accumulation: a raw signed overflow here is undefined behavior
  // (and, worse, UB whose visible result changes with the optimization
  // level -- see checkedAddI64). This saturates at the int64 boundary
  // instead of wrapping, so an accumulator that runs past ~92.23e9 units
  // (at the default 1e8 scale) reports the ceiling rather than a
  // wrong-signed garbage value.
  constexpr Decimal& operator+=(const Decimal& other)
  {
    FLOX_SCALE_CHECK(_raw == 0 || other._raw == 0 || scale() == other.scale(),
                     "Decimal::operator+= scale mismatch");
#if FLOX_SCALE_CHECKS
    if (_raw == 0)
    {
      _scale = other._scale;
    }
#endif
    _raw = checkedAddI64(_raw, other._raw);
    return *this;
  }

  constexpr Decimal& operator-=(const Decimal& other)
  {
    FLOX_SCALE_CHECK(_raw == 0 || other._raw == 0 || scale() == other.scale(),
                     "Decimal::operator-= scale mismatch");
#if FLOX_SCALE_CHECKS
    if (_raw == 0)
    {
      _scale = other._scale;
    }
#endif
    _raw = checkedSubI64(_raw, other._raw);
    return *this;
  }

  constexpr Decimal& operator*=(const Decimal& other)
  {
    *this = *this * other;
    return *this;
  }

  constexpr Decimal& operator/=(const Decimal& other)
  {
    *this = *this / other;
    return *this;
  }

  constexpr bool isZero() const { return _raw == 0; }

  // Formatted from the raw integer rather than through std::to_string(double).
  // std::to_string writes the decimal separator of the C locale, and
  // std::locale::global with a named locale sets that locale too, so on a host
  // configured for comma decimals every price and quantity in an order body
  // went to the venue as "60000,000000". The venue-facing shape is unchanged:
  // six fractional digits, the same as the printf "%f" std::to_string uses.
  std::string toString() const
  {
    static constexpr int kFractionDigits = 6;
    static constexpr uint64_t kFractionScale = 1000000;
    static_assert(static_cast<uint64_t>(Scale) <= (UINT64_MAX / kFractionScale),
                  "Scale too large to rescale to six fractional digits in 64 bits");

    const bool negative = _raw < 0;
    // Negated as unsigned: the most negative int64_t has no positive
    // counterpart.
    const uint64_t magnitude =
        negative ? (0ULL - static_cast<uint64_t>(_raw)) : static_cast<uint64_t>(_raw);
    const uint64_t scale = static_cast<uint64_t>(Scale);

    uint64_t whole = magnitude / scale;
    const uint64_t remainder = magnitude % scale;
    uint64_t fraction = (remainder * kFractionScale + scale / 2) / scale;
    if (fraction >= kFractionScale)
    {
      fraction -= kFractionScale;
      ++whole;
    }

    std::string out;
    out.reserve(24);
    if (negative)
    {
      out.push_back('-');
    }
    out.append(std::to_string(whole));
    out.push_back('.');
    for (uint64_t divisor = kFractionScale / 10; divisor > 0; divisor /= 10)
    {
      out.push_back(static_cast<char>('0' + (fraction / divisor) % 10));
    }
    return out;
  }

  // Construct a value carrying an explicit runtime scale. In release the
  // scale argument is ignored and this is just Decimal(raw).
  static constexpr Decimal withScale(int64_t raw, [[maybe_unused]] int64_t s)
  {
    Decimal d(raw);
#if FLOX_SCALE_CHECKS
    d._scale = s;
#endif
    return d;
  }

 private:
  int64_t _raw;
#if FLOX_SCALE_CHECKS
  int64_t _scale{Scale};
#endif
};

template <typename Tag, int Scale_, int64_t TickSize_>
std::ostream& operator<<(std::ostream& os, const Decimal<Tag, Scale_, TickSize_>& value)
{
  return os << value.toDouble();
}

}  // namespace flox
