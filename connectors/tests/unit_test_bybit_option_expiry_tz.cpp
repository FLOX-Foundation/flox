/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline protocol tests: a Bybit option symbol encodes a UTC expiry date,
 * so parsing it must produce the same instant on every host. The parser
 * builds the expiry with std::mktime, which reads the broken-down time as
 * *local* time, and reads the month with std::get_time("%b"), which is
 * locale-dependent -- so the same symbol registers a different expiry in
 * Singapore than in London, and under a non-English locale does not parse
 * at all and silently falls back to Spot.
 *
 * parseOptionSymbol is a free function with external linkage defined in
 * bybit_exchange_connector.cpp and declared in no header; it is
 * forward-declared here and resolved from libflox-connectors.a at link
 * time, the same way unit_test_bybit_option_symbol.cpp does it.
 */

#include "flox-connectors/bybit/bybit_exchange_connector.h"

#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/util/base/time.h>

#include <gtest/gtest.h>

#include <chrono>
#include <clocale>
#include <cstdlib>
#include <ctime>
#include <locale>
#include <optional>
#include <string>
#include <string_view>

namespace flox
{
std::optional<SymbolInfo> parseOptionSymbol(std::string_view fullSymbol,
                                            std::string_view exchange = "bybit");
}

using namespace flox;

namespace
{

// 2024-08-30T00:00:00Z. The symbol carries a date and nothing else, so the
// only defensible instant is midnight UTC on that date.
constexpr int64_t kExpectedExpiryUnixMs = 1724976000000LL;

// POSIX TZ string rather than a tzdata zone name: "XXX-14" is UTC+14 with no
// DST rule and needs no zoneinfo database, so it behaves identically on a
// developer machine and in a stripped CI container. 14 hours is the largest
// offset in use anywhere, which makes the shift impossible to mistake for
// rounding.
constexpr const char* kFarFromUtcTz = "XXX-14";

class ScopedTz
{
 public:
  explicit ScopedTz(const char* tz)
  {
    if (const char* prev = std::getenv("TZ"))
    {
      _prev = prev;
      _had = true;
    }
    ::setenv("TZ", tz, 1);
    ::tzset();
  }

  ~ScopedTz()
  {
    if (_had)
    {
      ::setenv("TZ", _prev.c_str(), 1);
    }
    else
    {
      ::unsetenv("TZ");
    }
    ::tzset();
  }

 private:
  std::string _prev;
  bool _had{false};
};

// Restores both the C++ global locale and the C locale: std::locale::global
// sets the C locale too when the locale is named, and std::to_string (used by
// Decimal::toString on every order body) reads that one.
class ScopedGlobalLocale
{
 public:
  ScopedGlobalLocale() : _prev(std::locale()) {}

  ~ScopedGlobalLocale()
  {
    std::locale::global(_prev);
    std::setlocale(LC_ALL, "C");
  }

  // Installs the first locale that exists on this host out of a set that all
  // write decimals with a comma and abbreviate August as something other than
  // "AUG". Returns the name installed, or an empty string if the host carries
  // none of them -- in which case the timezone half of the test still stands
  // on its own.
  std::string installCommaDecimal()
  {
    for (const char* name : {"fr_FR.UTF-8", "ru_RU.UTF-8", "it_IT.UTF-8", "es_ES.UTF-8",
                             "pt_BR.UTF-8", "fr_FR.utf8", "ru_RU.utf8", "it_IT.utf8"})
    {
      try
      {
        std::locale::global(std::locale(name));
        return name;
      }
      catch (const std::runtime_error&)
      {
        continue;
      }
    }
    return {};
  }

 private:
  std::locale _prev;
};

int64_t expiryNs(const SymbolInfo& info)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(info.expiry->time_since_epoch())
      .count();
}

}  // namespace

// Control: under the process's own settings the symbol parses and carries an
// expiry at all. Green today -- it is here so a failure of the two tests below
// cannot be read as "parseOptionSymbol is broken in general".
TEST(BybitOptionExpiryTz, ParsesUnderTheProcessDefaultSettings)
{
  auto info = parseOptionSymbol("BTC-30AUG24-50000-C", "bybit");
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->type, InstrumentType::Option);
  ASSERT_TRUE(info->expiry.has_value());
}

// The expiry is a property of the instrument, not of the machine that parsed
// it. Under TZ=UTC+14 std::mktime reads "30 Aug 2024 00:00:00" as local time
// and returns an instant 14 hours before midnight UTC, so the option is
// registered as expiring on 29 August -- a full day early for anything that
// compares expiry against a date.
TEST(BybitOptionExpiryTz, ExpiryIsUtcRegardlessOfProcessTimezone)
{
  ScopedTz tz(kFarFromUtcTz);

  auto info = parseOptionSymbol("BTC-30AUG24-50000-C", "bybit");
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->expiry.has_value());

  // Computed after the parse, so the lazy timebase mapping the parser
  // installs is already in place and both sides share the same offset; the
  // only difference this comparison can show is the timezone shift.
  const int64_t expected = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               fromUnixMs(kExpectedExpiryUnixMs).time_since_epoch())
                               .count();

  EXPECT_EQ(expiryNs(*info), expected)
      << "off by " << (expiryNs(*info) - expected) / 3'600'000'000'000LL
      << " hours under TZ=" << kFarFromUtcTz
      << " -- the date in the symbol is UTC and must not be read as local time";
}

// Same instant, now with the host's timezone *and* its locale moved: the
// month token in "30AUG24" is matched through std::get_time("%b"), which
// consults the global locale's month names. Under a locale that abbreviates
// August as anything else the parse fails outright, parseOptionSymbol returns
// nullopt, and resolveSymbolId registers the option as a Spot instrument with
// no strike, no expiry and no option type.
TEST(BybitOptionExpiryTz, ExpiryIsUtcRegardlessOfLocale)
{
  ScopedTz tz(kFarFromUtcTz);
  ScopedGlobalLocale loc;
  const std::string installed = loc.installCommaDecimal();

  auto info = parseOptionSymbol("BTC-30AUG24-50000-C", "bybit");
  ASSERT_TRUE(info.has_value())
      << "the symbol failed to parse under locale '" << installed
      << "' -- venue symbols are ASCII protocol tokens and must not be read through the host's "
         "month names";
  EXPECT_EQ(info->type, InstrumentType::Option);
  ASSERT_TRUE(info->expiry.has_value());

  const int64_t expected = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               fromUnixMs(kExpectedExpiryUnixMs).time_since_epoch())
                               .count();

  EXPECT_EQ(expiryNs(*info), expected) << "locale '" << installed << "', TZ=" << kFarFromUtcTz;
}
