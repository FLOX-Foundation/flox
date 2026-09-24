/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Strict parsers for the FIX fields decimal_wire.h does not cover: the integer
 * id fields (ClOrdID, OrigClOrdID, Account, Symbol) and UTCTimestamp.
 *
 * FIX types the id fields as String and this venue carries them as integers,
 * so every one of them has a set of wire values it cannot represent. The C
 * conversions are the wrong tool for that boundary: strtoull skips leading
 * whitespace, accepts a sign, stops at the first character it does not like
 * and reports the rest through errno, so "ORD-A1" becomes 0, "-1" becomes
 * UINT64_MAX and an overflowing value becomes UINT64_MAX as well -- three
 * different senders handed one id, none of them told. These return false
 * instead, and the caller refuses the message naming the field.
 *
 * The timestamp parse is here for the same reason plus one more: it must not
 * go through mktime/strptime, which read the process's timezone and locale. A
 * GTD expiry that moves with the venue host's /etc/localtime is not an
 * expiry -- the FIX UTCTimestamp is UTC by definition, so the civil-to-days
 * arithmetic is done here, in one place, with no clock or environment read.
 */
#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace flox::venue::fixfield
{

// Decimal digits only: no sign, no leading or trailing space, no 0x prefix,
// no decimal point, no empty string, and no value above UINT64_MAX.
//
// A leading zero is refused too (except for "0" itself), because this parse
// is what turns a client's chosen name into a venue order id: "0042" and "42"
// are two different ClOrdIDs, and accepting both would hand them one id --
// the same failure the rest of this header exists to prevent, arrived at from
// the other side.
inline bool parseU64(std::string_view t, uint64_t& out) noexcept
{
  if (t.empty() || (t.size() > 1 && t[0] == '0'))
  {
    return false;
  }
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  uint64_t v = 0;
  for (const char c : t)
  {
    if (c < '0' || c > '9')
    {
      return false;
    }
    const uint64_t d = static_cast<uint64_t>(c - '0');
    if (v > (kMax - d) / 10)
    {
      return false;  // the next multiply would wrap
    }
    v = v * 10 + d;
  }
  out = v;
  return true;
}

// Same, narrowed to the width SymbolId actually has: a value above UINT32_MAX
// truncated into a SymbolId names a different instrument, which is worse than
// a refusal the sender can read.
inline bool parseU32(std::string_view t, uint32_t& out) noexcept
{
  uint64_t v = 0;
  if (!parseU64(t, v) || v > std::numeric_limits<uint32_t>::max())
  {
    return false;
  }
  out = static_cast<uint32_t>(v);
  return true;
}

namespace detail
{

// Days between 1970-01-01 and the given proleptic-Gregorian civil date, by
// Howard Hinnant's days_from_civil. Pure arithmetic: no tm, no timezone, no
// locale, and the same answer on every host.
constexpr int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) noexcept
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

constexpr unsigned daysInMonth(int64_t y, unsigned m) noexcept
{
  constexpr unsigned kLen[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
  {
    return 29;
  }
  return kLen[m - 1];
}

constexpr bool digits(std::string_view t, size_t from, size_t n, unsigned& out) noexcept
{
  unsigned v = 0;
  for (size_t i = from; i < from + n; ++i)
  {
    if (t[i] < '0' || t[i] > '9')
    {
      return false;
    }
    v = v * 10 + static_cast<unsigned>(t[i] - '0');
  }
  out = v;
  return true;
}

}  // namespace detail

// FIX UTCTimestamp -- YYYYMMDD-HH:MM:SS with optional .sss milliseconds -- to
// nanoseconds since the Unix epoch, read as UTC. The inverse of
// flox::fix::sendingTime (tag 52), which prints exactly this shape.
//
// Refused: any other length or punctuation, a non-digit anywhere, a month
// outside 1-12, a day the month does not have (so 20260229 is refused and
// 20240229 is not), an hour above 23, a minute above 59, and a year before
// 1970 -- the venue's timestamps are unsigned time since the epoch, and a
// sub-epoch expiry is a typo, not a date -- and a year after 2261, past which
// nanoseconds since the epoch no longer fit an int64. Seconds may be 60: FIX 4.4 allows
// the leap second, and it lands on the following minute the way a POSIX epoch
// count does.
inline bool parseUtcTimestampNs(std::string_view t, int64_t& outNs) noexcept
{
  constexpr size_t kNoMillis = 17;  // YYYYMMDD-HH:MM:SS
  constexpr size_t kMillis = 21;    // YYYYMMDD-HH:MM:SS.sss
  if (t.size() != kNoMillis && t.size() != kMillis)
  {
    return false;
  }
  if (t[8] != '-' || t[11] != ':' || t[14] != ':')
  {
    return false;
  }
  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  unsigned millis = 0;
  if (!detail::digits(t, 0, 4, year) || !detail::digits(t, 4, 2, month) ||
      !detail::digits(t, 6, 2, day) || !detail::digits(t, 9, 2, hour) ||
      !detail::digits(t, 12, 2, minute) || !detail::digits(t, 15, 2, second))
  {
    return false;
  }
  if (t.size() == kMillis)
  {
    if (t[17] != '.' || !detail::digits(t, 18, 3, millis))
    {
      return false;
    }
  }
  // 2262-04-11 is where int64 nanoseconds run out; the year bound keeps the
  // multiplication below from overflowing on any accepted date.
  if (year < 1970 || year > 2261 || month < 1 || month > 12 || day < 1 ||
      day > detail::daysInMonth(year, month) || hour > 23 || minute > 59 || second > 60)
  {
    return false;
  }
  const int64_t days = detail::daysFromCivil(year, month, day);
  const int64_t secs = days * 86400 + static_cast<int64_t>(hour) * 3600 +
                       static_cast<int64_t>(minute) * 60 + static_cast<int64_t>(second);
  outNs = secs * 1'000'000'000 + static_cast<int64_t>(millis) * 1'000'000;
  return true;
}

}  // namespace flox::venue::fixfield
