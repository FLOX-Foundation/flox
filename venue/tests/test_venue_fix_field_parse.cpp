/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Unit tests for the strict FIX field parsers (flox-venue/fix_field_parse.h).
 * test_venue_fix_codec_strictness.cpp holds the codec to what it does with
 * these answers; this file holds the answers themselves, including the cases
 * the codec has no wire value for -- every shape strtoull/strtoul used to
 * accept silently, and the calendar and timezone edges of the timestamp.
 */
#include "flox-venue/fix_field_parse.h"

#include "flox/connector/fix/fix_wire.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>

using namespace flox::venue::fixfield;

namespace
{

TEST(FixFieldParseU64, DecimalDigitsRoundTrip)
{
  uint64_t v = 123;
  EXPECT_TRUE(parseU64("0", v));
  EXPECT_EQ(v, 0ULL);
  EXPECT_TRUE(parseU64("7", v));
  EXPECT_EQ(v, 7ULL);
  EXPECT_TRUE(parseU64("18446744073709551615", v));
  EXPECT_EQ(v, UINT64_MAX);
}

// Every one of these is a value strtoull turns into a number: it skips the
// space, honours the sign, wraps the negation, reads "0x10" as 16 and stops
// at the first junk character, reporting nothing the call site reads.
TEST(FixFieldParseU64, ShapesStrtoullWouldHaveAccepted)
{
  uint64_t v = 0;
  for (const char* t : {"", " 42", "42 ", "+42", "-1", "-0", "0x10", "4.2", "4e2", "123ABC",
                        "ORD-A1", "\t7", "1 000", "１２３"})
  {
    EXPECT_FALSE(parseU64(t, v)) << "accepted \"" << t << "\"";
  }
}

// One digit past UINT64_MAX, and the 20-digit value the review found landing
// on UINT64_MAX through ERANGE.
TEST(FixFieldParseU64, OverflowIsRefusedRatherThanSaturated)
{
  uint64_t v = 0;
  EXPECT_FALSE(parseU64("18446744073709551616", v));
  EXPECT_FALSE(parseU64("99999999999999999999", v));
  EXPECT_FALSE(parseU64("184467440737095516150", v));
  EXPECT_TRUE(parseU64("18446744073709551615", v));
  EXPECT_EQ(v, UINT64_MAX);
}

// A zero-padded id is a different string from the unpadded one; folding both
// onto one number is the collision this parse exists to refuse.
TEST(FixFieldParseU64, LeadingZeroIsRefusedExceptForZeroItself)
{
  uint64_t v = 0;
  EXPECT_TRUE(parseU64("0", v));
  EXPECT_FALSE(parseU64("00", v));
  EXPECT_FALSE(parseU64("0042", v));
  EXPECT_FALSE(parseU64("018446744073709551615", v));
}

TEST(FixFieldParseU32, NarrowsAtTheSymbolIdWidth)
{
  uint32_t v = 0;
  EXPECT_TRUE(parseU32("0", v));
  EXPECT_EQ(v, 0U);
  EXPECT_TRUE(parseU32("4294967295", v));
  EXPECT_EQ(v, UINT32_MAX);
  EXPECT_FALSE(parseU32("4294967296", v));  // truncates to 0 on a cast: a different book
  EXPECT_FALSE(parseU32("18446744073709551615", v));
  EXPECT_FALSE(parseU32("BTC-USD", v));
  EXPECT_EQ(v, UINT32_MAX) << "a refused parse must not write the output";
}

TEST(FixFieldParseTimestamp, IsTheInverseOfSendingTime)
{
  // Both shapes of the same instant, and the round trip through the printer
  // both ends of a session share.
  for (const char* t : {"20260925-12:00:00.000", "19700101-00:00:00.000", "20240229-23:59:59.999",
                        "20261231-00:00:00.001"})
  {
    int64_t ns = -1;
    ASSERT_TRUE(parseUtcTimestampNs(t, ns)) << t;
    EXPECT_EQ(flox::fix::sendingTime(ns), std::string(t));
  }

  int64_t ns = -1;
  ASSERT_TRUE(parseUtcTimestampNs("20260925-12:00:00", ns));
  EXPECT_EQ(ns, 1790337600000000000LL) << "the milliseconds are optional, the instant is not";
  ASSERT_TRUE(parseUtcTimestampNs("20260925-12:00:00.250", ns));
  EXPECT_EQ(ns, 1790337600250000000LL);
  ASSERT_TRUE(parseUtcTimestampNs("19700101-00:00:00.000", ns));
  EXPECT_EQ(ns, 0LL);
}

// The reason this does not go through strptime/mktime: those read TZ from the
// environment, so the same wire value would mean a different instant on a
// venue host in Singapore than on one in UTC. Set TZ to something far from UTC
// and the answer must not move.
namespace
{
// setenv/unsetenv are POSIX; Windows spells them _putenv_s, and its tzset is
// _tzset. Same effect: the parser under test must not read either.
void setTz(const char* value)
{
#ifdef _WIN32
  _putenv_s("TZ", value);
  _tzset();
#else
  setenv("TZ", value, 1);
  tzset();
#endif
}

void clearTz()
{
#ifdef _WIN32
  _putenv_s("TZ", "");
  _tzset();
#else
  unsetenv("TZ");
  tzset();
#endif
}
}  // namespace

TEST(FixFieldParseTimestamp, DoesNotDependOnTheHostTimezone)
{
  const char* saved = std::getenv("TZ");
  const std::string savedTz = saved != nullptr ? saved : std::string{};

  int64_t utc = -1;
  ASSERT_TRUE(parseUtcTimestampNs("20260925-12:00:00.000", utc));

  for (const char* tz : {"Asia/Singapore", "America/New_York", "Pacific/Kiritimati"})
  {
    setTz(tz);
    int64_t ns = -1;
    ASSERT_TRUE(parseUtcTimestampNs("20260925-12:00:00.000", ns)) << tz;
    EXPECT_EQ(ns, utc) << tz;
  }

  if (saved != nullptr)
  {
    setTz(savedTz.c_str());
  }
  else
  {
    clearTz();
  }
}

TEST(FixFieldParseTimestamp, MalformedShapesRefused)
{
  int64_t ns = 0;
  for (const char* t : {"",
                        "not-a-time",
                        "20260925",                 // date only
                        "20260925-12:00",           // no seconds
                        "20260925-12:00:00.",       // a dot and no millis
                        "20260925-12:00:00.25",     // two millis digits
                        "20260925-12:00:00.2500",   // four
                        "20260925T12:00:00.000",    // ISO separator, not FIX
                        "20260925-12-00-00.000",    // wrong time punctuation
                        "2026-09-25-12:00:00.000",  // wrong date punctuation
                        " 20260925-12:00:00.000",
                        "20260925-12:00:00.000 ",
                        "20260925-12:00:00.00a"})
  {
    EXPECT_FALSE(parseUtcTimestampNs(t, ns)) << "accepted \"" << t << "\"";
  }
}

TEST(FixFieldParseTimestamp, CalendarAndClockRangesEnforced)
{
  int64_t ns = 0;
  EXPECT_FALSE(parseUtcTimestampNs("20261325-12:00:00.000", ns)) << "month 13";
  EXPECT_FALSE(parseUtcTimestampNs("20260025-12:00:00.000", ns)) << "month 0";
  EXPECT_FALSE(parseUtcTimestampNs("20260900-12:00:00.000", ns)) << "day 0";
  EXPECT_FALSE(parseUtcTimestampNs("20260931-12:00:00.000", ns)) << "September has 30 days";
  EXPECT_FALSE(parseUtcTimestampNs("20260229-12:00:00.000", ns)) << "2026 is not a leap year";
  EXPECT_FALSE(parseUtcTimestampNs("21000229-12:00:00.000", ns)) << "2100 is not a leap year";
  EXPECT_TRUE(parseUtcTimestampNs("20000229-12:00:00.000", ns)) << "2000 is";
  EXPECT_FALSE(parseUtcTimestampNs("20260925-24:00:00.000", ns)) << "hour 24";
  EXPECT_FALSE(parseUtcTimestampNs("20260925-12:60:00.000", ns)) << "minute 60";
  EXPECT_FALSE(parseUtcTimestampNs("20260925-12:00:61.000", ns)) << "second 61";
  EXPECT_TRUE(parseUtcTimestampNs("20260925-12:00:60.000", ns)) << "the FIX 4.4 leap second";
  EXPECT_FALSE(parseUtcTimestampNs("19691231-23:59:59.999", ns)) << "before the epoch";
  EXPECT_TRUE(parseUtcTimestampNs("22611231-23:59:59.999", ns)) << "the last year whose nanoseconds fit an int64";
  EXPECT_FALSE(parseUtcTimestampNs("22620101-00:00:00", ns)) << "a year past the int64 nanosecond range";
  EXPECT_FALSE(parseUtcTimestampNs("99991231-23:59:59.999", ns)) << "the last four-digit year overflows";
}

}  // namespace
