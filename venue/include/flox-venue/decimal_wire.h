/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Decimal <-> fixed-point wire helpers, shared by the venue text codecs
 * (REST/JSON and FIX). Parsing goes straight from a decimal string into the
 * engine's fixed-point raw value with NO double round-trip: exponents, signs,
 * excess precision, overflow, empty and trailing garbage are all rejected
 * rather than silently coerced. Printing emits the exact minimal decimal (no
 * trailing zeros, no fixed "%f" padding).
 *
 * Price::Scale and Quantity::Scale are the same power of ten, so one scale
 * serves both.
 */
#pragma once

#include "flox/common.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace flox::venue::decwire
{

static_assert(Price::Scale == Quantity::Scale, "shared fixed-point scale");

inline constexpr int64_t kScale = Price::Scale;
inline constexpr int kFracDigits = []
{
  int d = 0;
  for (int64_t s = kScale; s > 1; s /= 10)
  {
    ++d;
  }
  return d;
}();

// Unsigned decimal string -> fixed-point raw at kScale. Returns false on an
// empty string, a sign, an exponent, more fractional digits than the scale
// carries, trailing non-digits, or overflow.
inline bool parse(std::string_view t, int64_t& out)
{
  size_t i = 0;
  uint64_t ip = 0;
  size_t intDigits = 0;
  while (i < t.size() && t[i] >= '0' && t[i] <= '9')
  {
    ip = ip * 10 + static_cast<uint64_t>(t[i] - '0');
    if (ip > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / kScale))
    {
      return false;
    }
    ++i;
    ++intDigits;
  }
  if (intDigits == 0)
  {
    return false;
  }
  uint64_t frac = 0;
  int fracDigits = 0;
  if (i < t.size() && t[i] == '.')
  {
    ++i;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9')
    {
      if (fracDigits == kFracDigits)
      {
        return false;  // more precision than the fixed-point carries
      }
      frac = frac * 10 + static_cast<uint64_t>(t[i] - '0');
      ++fracDigits;
      ++i;
    }
    if (fracDigits == 0)
    {
      return false;
    }
  }
  if (i != t.size())
  {
    return false;  // sign, exponent, or other trailing characters
  }
  while (fracDigits < kFracDigits)
  {
    frac *= 10;
    ++fracDigits;
  }
  const uint64_t scaled = ip * static_cast<uint64_t>(kScale);
  if (scaled > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - frac)
  {
    return false;
  }
  out = static_cast<int64_t>(scaled + frac);
  return true;
}

inline void appendU64(std::string& o, uint64_t v)
{
  char buf[20];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  o.append(buf, static_cast<size_t>(r.ptr - buf));
}

// Append raw/kScale as an exact minimal decimal (trailing zeros trimmed).
inline void append(std::string& o, int64_t raw)
{
  uint64_t u;
  if (raw < 0)
  {
    o.push_back('-');
    u = ~static_cast<uint64_t>(raw) + 1;
  }
  else
  {
    u = static_cast<uint64_t>(raw);
  }
  appendU64(o, u / static_cast<uint64_t>(kScale));
  uint64_t frac = u % static_cast<uint64_t>(kScale);
  if (frac != 0)
  {
    char digits[kFracDigits];
    for (int d = kFracDigits - 1; d >= 0; --d)
    {
      digits[d] = static_cast<char>('0' + frac % 10);
      frac /= 10;
    }
    int len = kFracDigits;
    while (len > 0 && digits[len - 1] == '0')
    {
      --len;
    }
    o.push_back('.');
    o.append(digits, static_cast<size_t>(len));
  }
}

}  // namespace flox::venue::decwire
