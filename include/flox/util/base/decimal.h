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
      return Decimal(static_cast<int64_t>(val >= 0.0
                                              ? val * Scale + 0.5
                                              : val * Scale - 0.5));
    }
    else
    {
      return Decimal(0);
    }
  }
  static constexpr Decimal fromRaw(int64_t raw) { return Decimal(raw); }

  constexpr double toDouble() const { return static_cast<double>(_raw) / Scale; }

  // Scale-aware conversions (W17-T001). The compile-time Scale is the default
  // (1e8) used by CEX symbols; a DEX symbol carries its own scale in
  // SymbolInfo and converts through these explicit-scale overloads so the
  // raw int64 is interpreted correctly for its value range.
  constexpr double toDouble(int64_t scale) const
  {
    return static_cast<double>(_raw) / static_cast<double>(scale);
  }
  static constexpr Decimal fromDouble(double val, int64_t scale)
  {
    return withScale(static_cast<int64_t>(val >= 0.0 ? val * scale + 0.5 : val * scale - 0.5),
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
  constexpr Decimal rescale(int64_t fromScale, int64_t toScale) const
  {
#if defined(__SIZEOF_INT128__)
    using i128 = __int128_t;
    return withScale(checkedNarrowI64((i128)_raw * (i128)toScale / (i128)fromScale), toScale);
#else
    return withScale(static_cast<int64_t>(_raw * toScale / fromScale), toScale);
#endif
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
    return withScale(_raw + d._raw, _raw != 0 ? scale() : d.scale());
  }
  constexpr Decimal operator-(Decimal d) const
  {
    FLOX_SCALE_CHECK(_raw == 0 || d._raw == 0 || scale() == d.scale(),
                     "Decimal::operator- scale mismatch");
    return withScale(_raw - d._raw, _raw != 0 ? scale() : d.scale());
  }

  // Scalar multiply / divide preserve the value's scale.
  constexpr Decimal operator*(int64_t x) const { return withScale(_raw * x, scale()); }
  constexpr Decimal operator/(int64_t x) const { return withScale(_raw / x, scale()); }

  // Fixed-point multiply / divide bake in the compile-time Scale, so both
  // operands must be at the default scale; rescale a per-symbol value first.
  // The result is at the default scale.
  constexpr Decimal operator*(const Decimal& other) const
  {
    FLOX_SCALE_CHECK(scale() == Scale && other.scale() == Scale,
                     "Decimal::operator* requires default scale; rescale first");
#if defined(__SIZEOF_INT128__)
    using i128 = __int128_t;
    return Decimal(checkedNarrowI64((i128)_raw * (i128)other._raw / (i128)Scale));
#else
    return Decimal((_raw / Scale) * other._raw + (_raw % Scale) * other._raw / Scale);
#endif
  }
  constexpr Decimal operator/(const Decimal& other) const
  {
    assert(other.isZero() != 0 && "Division by zero");
    FLOX_SCALE_CHECK(scale() == Scale && other.scale() == Scale,
                     "Decimal::operator/ requires default scale; rescale first");
#if defined(__SIZEOF_INT128__)
    using i128 = __int128_t;
    return Decimal(checkedNarrowI64(((i128)_raw * (i128)Scale) / (i128)other._raw));
#else
    return Decimal((_raw * Scale) / other._raw);
#endif
  }

  constexpr friend Decimal operator*(int64_t x, Decimal d) { return withScale(x * d._raw, d.scale()); }

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
    _raw += other._raw;
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
    _raw -= other._raw;
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

  std::string toString() const
  {
    return std::to_string(toDouble());
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
