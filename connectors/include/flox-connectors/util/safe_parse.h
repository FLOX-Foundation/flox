/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

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
