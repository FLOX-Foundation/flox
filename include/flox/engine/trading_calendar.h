/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>

// Trading calendar: when is a market open? Crypto runs 24/7; equities and
// futures trade only during a session. A backtest that fills an equity order at
// 3am is fiction, so the harness consults a calendar before submitting.
//
// The session is expressed in UTC minutes-of-day plus a weekday mask, so it is
// deterministic and needs no timezone database. DST is intentionally out of
// scope: usEquityRegularHours() encodes the EST window (UTC-5); for a
// DST-exact backtest, build an explicit session() per period.

namespace flox
{

class TradingCalendar
{
 public:
  // Default: 24/7 (crypto).
  TradingCalendar() = default;

  static TradingCalendar always() { return TradingCalendar(); }

  // A weekday session window in UTC minutes-of-day [openMinUtc, closeMinUtc),
  // active on weekdayMask (bit 0 = Sunday, bit 1 = Monday, ... bit 6 = Saturday).
  static TradingCalendar session(int openMinUtc, int closeMinUtc, uint8_t weekdayMask)
  {
    TradingCalendar c;
    c._always = false;
    c._openMin = openMinUtc;
    c._closeMin = closeMinUtc;
    c._weekdayMask = weekdayMask;
    return c;
  }

  // US equity regular hours: 09:30-16:00 ET, Monday-Friday. Encoded in UTC as
  // 14:30-21:00 (EST, UTC-5). 0x3E = Mon..Fri.
  static TradingCalendar usEquityRegularHours() { return session(14 * 60 + 30, 21 * 60, 0x3E); }

  bool isAlwaysOpen() const { return _always; }

  // True when the market is open at the given UTC timestamp (nanoseconds since
  // the Unix epoch). A 24/7 calendar is always open; a session calendar checks
  // weekday membership and the minute-of-day window.
  bool isOpen(int64_t tsNs) const
  {
    if (_always)
    {
      return true;
    }
    if (tsNs < 0)
    {
      return false;
    }
    constexpr int64_t kNsPerDay = 86'400'000'000'000LL;
    constexpr int64_t kNsPerMinute = 60'000'000'000LL;
    const int64_t days = tsNs / kNsPerDay;
    const int64_t minuteOfDay = (tsNs % kNsPerDay) / kNsPerMinute;
    // 1970-01-01 was a Thursday; with Sunday = 0 that is weekday 4.
    const int dow = static_cast<int>(((days % 7) + 4) % 7);
    if ((_weekdayMask & (1u << dow)) == 0)
    {
      return false;
    }
    return minuteOfDay >= _openMin && minuteOfDay < _closeMin;
  }

 private:
  bool _always{true};
  int _openMin{0};
  int _closeMin{1440};
  uint8_t _weekdayMask{0x7F};
};

}  // namespace flox
