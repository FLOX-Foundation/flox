/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <flox/common.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace flox
{
namespace util
{

/// Parse double from string_view.
/// Uses std::from_chars where available (GCC/MSVC), falls back to strtod on Apple clang.
/// Returns std::nullopt on:
/// - Empty input
/// - Invalid format
/// - Overflow/underflow
/// - Partial parse (not all characters consumed)
inline std::optional<double> safeParseDouble(std::string_view sv)
{
  if (sv.empty())
  {
    return std::nullopt;
  }

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && !defined(__APPLE__) && \
    !defined(_LIBCPP_VERSION)
  // Fast path: std::from_chars for double (GCC 11+, MSVC, libstdc++)
  double result{};
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);

  if (ec != std::errc{})
  {
    return std::nullopt;
  }

  if (ptr != sv.data() + sv.size())
  {
    return std::nullopt;
  }

  return result;
#else
  // Fallback
  std::string str(sv);
  char* end = nullptr;
  double result = std::strtod(str.c_str(), &end);

  if (end == str.c_str())
  {
    return std::nullopt;
  }

  if (static_cast<size_t>(end - str.c_str()) != str.size())
  {
    return std::nullopt;  // Partial parse
  }

  return result;
#endif
}

/// Parse a decimal string straight into a fixed-point raw value at `scale`
/// (a power of ten, e.g. Price::Scale). NO double round-trip and no allocation.
///
/// Lenient by design: exchange market data is authoritative, so this ACCEPTS an
/// over-precise value by truncating fractional digits beyond the scale (toward
/// zero) rather than rejecting it -- dropping a book level because the venue
/// sent one extra digit would be worse than a sub-tick rounding. This is the
/// deliberate opposite of the venue's strict decwire parser, which rejects
/// untrusted client input. Accepts an optional leading sign.
///
/// Returns false only on: empty input, no digits, an integer part that would
/// overflow the raw range, or trailing non-numeric characters (including
/// exponent notation, which crypto books do not use for price/size).
inline bool parseFixedPoint(std::string_view t, int64_t scale, int64_t& out)
{
  if (t.empty() || scale <= 0)
  {
    return false;
  }
  size_t i = 0;
  bool neg = false;
  if (t[i] == '+' || t[i] == '-')
  {
    neg = (t[i] == '-');
    ++i;
  }

  int fracDigits = 0;
  for (int64_t s = scale; s > 1; s /= 10)
  {
    ++fracDigits;
  }

  const uint64_t kMaxInt = static_cast<uint64_t>(INT64_MAX) / static_cast<uint64_t>(scale);
  uint64_t ip = 0;
  size_t intDigits = 0;
  for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i)
  {
    ip = ip * 10 + static_cast<uint64_t>(t[i] - '0');
    if (ip > kMaxInt)
    {
      return false;  // integer part alone would overflow the raw value
    }
    ++intDigits;
  }

  uint64_t frac = 0;
  int got = 0;
  if (i < t.size() && t[i] == '.')
  {
    ++i;
    for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i)
    {
      if (got < fracDigits)
      {
        frac = frac * 10 + static_cast<uint64_t>(t[i] - '0');
        ++got;
      }
      // digits beyond the fixed-point scale are truncated, not rejected
    }
  }

  if (intDigits == 0 && got == 0)
  {
    return false;  // no numeric digits
  }
  if (i != t.size())
  {
    return false;  // stray trailing characters (e.g. exponent)
  }
  while (got < fracDigits)
  {
    frac *= 10;
    ++got;
  }

  const uint64_t raw = ip * static_cast<uint64_t>(scale) + frac;
  if (raw > static_cast<uint64_t>(INT64_MAX))
  {
    return false;
  }
  out = neg ? -static_cast<int64_t>(raw) : static_cast<int64_t>(raw);
  return true;
}

/// Convenience: parse a decimal string directly into a Price (fixed-point, no
/// double). nullopt on a malformed value; excess precision is truncated.
inline std::optional<Price> parsePrice(std::string_view sv)
{
  int64_t raw = 0;
  if (!parseFixedPoint(sv, Price::Scale, raw))
  {
    return std::nullopt;
  }
  return Price::fromRaw(raw);
}

/// Convenience: parse a decimal string directly into a Quantity (fixed-point,
/// no double). nullopt on a malformed value; excess precision is truncated.
inline std::optional<Quantity> parseQty(std::string_view sv)
{
  int64_t raw = 0;
  if (!parseFixedPoint(sv, Quantity::Scale, raw))
  {
    return std::nullopt;
  }
  return Quantity::fromRaw(raw);
}

/// Parse int64_t from string_view using std::from_chars (fast, no allocation).
/// Returns std::nullopt on:
/// - Empty input
/// - Invalid format
/// - Overflow/underflow
/// - Partial parse
inline std::optional<int64_t> parseInt64(std::string_view sv, int base = 10)
{
  if (sv.empty())
  {
    return std::nullopt;
  }

  int64_t result{};
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result, base);

  if (ec != std::errc{})
  {
    return std::nullopt;
  }

  if (ptr != sv.data() + sv.size())
  {
    return std::nullopt;
  }

  return result;
}

/// Parse uint64_t from string_view using std::from_chars (fast, no allocation).
/// Returns std::nullopt on:
/// - Empty input
/// - Invalid format
/// - Overflow
/// - Partial parse
inline std::optional<uint64_t> parseUint64(std::string_view sv, int base = 10)
{
  if (sv.empty())
  {
    return std::nullopt;
  }

  uint64_t result{};
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result, base);

  if (ec != std::errc{})
  {
    return std::nullopt;
  }

  if (ptr != sv.data() + sv.size())
  {
    return std::nullopt;
  }

  return result;
}

}  // namespace util
}  // namespace flox
