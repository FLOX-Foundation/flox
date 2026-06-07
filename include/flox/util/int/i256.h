/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/util/int/u256.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace flox
{

// Signed 256-bit integer for chain-faithful fixed-point math that needs signs
// (Balancer's LogExpMath ln/exp). Sign and magnitude on top of u256. Division
// truncates toward zero, matching Solidity int256 `/` and `%` (which is what the
// contract relies on), not floor. All magnitudes used here fit in u256 because
// the contract itself stays within int256 and reverts otherwise.
struct i256
{
  u256 mag{};
  bool neg = false;

  i256() = default;
  i256(int64_t v) : mag(v < 0 ? u256(static_cast<uint64_t>(-v)) : u256(static_cast<uint64_t>(v))),
                    neg(v < 0)
  {
  }
  explicit i256(const u256& m, bool n = false) : mag(m), neg(n && !m.isZero()) {}

  static i256 fromDec(const std::string& s)
  {
    if (!s.empty() && s[0] == '-')
    {
      return i256(u256::fromDec(s.substr(1)), true);
    }
    return i256(u256::fromDec(s), false);
  }

  bool isZero() const { return mag.isZero(); }
  const u256& magnitude() const { return mag; }

  i256 operator-() const { return i256(mag, !neg); }

  friend bool operator==(const i256& a, const i256& b)
  {
    return a.mag == b.mag && (a.neg == b.neg || a.mag.isZero());
  }
  friend bool operator!=(const i256& a, const i256& b) { return !(a == b); }
  friend bool operator<(const i256& a, const i256& b)
  {
    if (a.neg != b.neg)
    {
      return a.neg;  // negative < non-negative
    }
    return a.neg ? (b.mag < a.mag) : (a.mag < b.mag);
  }
  friend bool operator>(const i256& a, const i256& b) { return b < a; }
  friend bool operator<=(const i256& a, const i256& b) { return !(b < a); }
  friend bool operator>=(const i256& a, const i256& b) { return !(a < b); }

  friend i256 operator+(const i256& a, const i256& b)
  {
    if (a.neg == b.neg)
    {
      return i256(a.mag + b.mag, a.neg);
    }
    // Opposite signs: subtract the smaller magnitude from the larger.
    if (a.mag >= b.mag)
    {
      return i256(a.mag - b.mag, a.neg);
    }
    return i256(b.mag - a.mag, b.neg);
  }
  friend i256 operator-(const i256& a, const i256& b) { return a + (-b); }
  friend i256 operator*(const i256& a, const i256& b)
  {
    return i256(a.mag * b.mag, a.neg != b.neg);
  }
  // Truncates toward zero (magnitude floor, sign is the xor), like Solidity.
  friend i256 operator/(const i256& a, const i256& b)
  {
    return i256(a.mag / b.mag, a.neg != b.neg);
  }
  friend i256 operator%(const i256& a, const i256& b) { return a - (a / b) * b; }
};

}  // namespace flox
