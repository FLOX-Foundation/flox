#include <gtest/gtest.h>

#include "flox/engine/trading_calendar.h"

using namespace flox;

namespace
{
constexpr int64_t kNsPerSec = 1'000'000'000LL;

// 2024-01-03 00:00:00 UTC (a Wednesday).
constexpr int64_t kWedMidnight = 1'704'240'000LL * kNsPerSec;
int64_t atUtc(int64_t dayMidnightNs, int hour, int minute)
{
  return dayMidnightNs + (static_cast<int64_t>(hour) * 3600 + minute * 60) * kNsPerSec;
}
}  // namespace

// A 24/7 calendar is open at every timestamp, including weekends and 3am.
TEST(TradingCalendarTest, AlwaysOpenIs247)
{
  const auto cal = TradingCalendar::always();
  EXPECT_TRUE(cal.isAlwaysOpen());
  EXPECT_TRUE(cal.isOpen(atUtc(kWedMidnight, 3, 0)));    // 3am
  EXPECT_TRUE(cal.isOpen(atUtc(kWedMidnight, 23, 59)));  // late
  const int64_t saturday = kWedMidnight + 3LL * 86'400 * kNsPerSec;
  EXPECT_TRUE(cal.isOpen(atUtc(saturday, 12, 0)));  // weekend
}

// US equity RTH: open during 14:30-21:00 UTC on a weekday, closed off-hours.
TEST(TradingCalendarTest, UsEquityOpenDuringSession)
{
  const auto cal = TradingCalendar::usEquityRegularHours();
  EXPECT_FALSE(cal.isAlwaysOpen());

  EXPECT_TRUE(cal.isOpen(atUtc(kWedMidnight, 15, 0)));   // 10:00 ET — open
  EXPECT_TRUE(cal.isOpen(atUtc(kWedMidnight, 14, 30)));  // 09:30 ET — open at the bell
  EXPECT_FALSE(cal.isOpen(atUtc(kWedMidnight, 21, 0)));  // 16:00 ET — closed at the close
  EXPECT_FALSE(cal.isOpen(atUtc(kWedMidnight, 9, 0)));   // pre-market
  EXPECT_FALSE(cal.isOpen(atUtc(kWedMidnight, 22, 0)));  // after hours
}

// The session is closed on weekends regardless of the time of day.
TEST(TradingCalendarTest, UsEquityClosedOnWeekend)
{
  const auto cal = TradingCalendar::usEquityRegularHours();
  const int64_t saturday = kWedMidnight + 3LL * 86'400 * kNsPerSec;  // 2024-01-06
  const int64_t sunday = kWedMidnight + 4LL * 86'400 * kNsPerSec;    // 2024-01-07
  EXPECT_FALSE(cal.isOpen(atUtc(saturday, 15, 0)));
  EXPECT_FALSE(cal.isOpen(atUtc(sunday, 15, 0)));
}

// A custom session honours its own window and weekday mask.
TEST(TradingCalendarTest, CustomSessionWindow)
{
  // Open 08:00-12:00 UTC, weekdays only (Mon..Fri = 0x3E).
  const auto cal = TradingCalendar::session(8 * 60, 12 * 60, 0x3E);
  EXPECT_TRUE(cal.isOpen(atUtc(kWedMidnight, 9, 0)));    // inside window
  EXPECT_FALSE(cal.isOpen(atUtc(kWedMidnight, 13, 0)));  // after window
  const int64_t saturday = kWedMidnight + 3LL * 86'400 * kNsPerSec;
  EXPECT_FALSE(cal.isOpen(atUtc(saturday, 9, 0)));  // weekend excluded by mask
}
