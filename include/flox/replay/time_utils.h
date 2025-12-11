/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

namespace flox::replay
{

class TimeUtils
{
 public:
  static constexpr int64_t kNsPerUs = 1'000LL;
  static constexpr int64_t kNsPerMs = 1'000'000LL;
  static constexpr int64_t kNsPerSec = 1'000'000'000LL;
  static constexpr int64_t kNsPerMin = 60LL * kNsPerSec;
  static constexpr int64_t kNsPerHour = 60LL * kNsPerMin;
  static constexpr int64_t kNsPerDay = 24LL * kNsPerHour;

  static std::optional<int64_t> parseISO8601(std::string_view str)
  {
    if (str.empty())
    {
      return std::nullopt;
    }

    static const std::regex iso_regex(
        R"((\d{4})-(\d{2})-(\d{2}))"
        R"((?:[T ](\d{2}):(\d{2}):(\d{2}))?)"
        R"((?:\.(\d{1,9}))?)"
        R"((?:Z|([+-])(\d{2}):?(\d{2}))?)");

    std::string input(str);
    std::smatch match;
    if (!std::regex_match(input, match, iso_regex))
    {
      return std::nullopt;
    }

    std::tm tm{};
    tm.tm_year = std::stoi(match[1].str()) - 1900;
    tm.tm_mon = std::stoi(match[2].str()) - 1;
    tm.tm_mday = std::stoi(match[3].str());

    if (match[4].matched)
    {
      tm.tm_hour = std::stoi(match[4].str());
      tm.tm_min = std::stoi(match[5].str());
      tm.tm_sec = std::stoi(match[6].str());
    }

    tm.tm_isdst = 0;
#ifdef _WIN32
    time_t epoch = _mkgmtime(&tm);
#else
    time_t epoch = timegm(&tm);
#endif

    if (epoch == -1)
    {
      return std::nullopt;
    }

    int64_t ns = static_cast<int64_t>(epoch) * kNsPerSec;

    if (match[7].matched)
    {
      std::string frac = match[7].str();
      while (frac.length() < 9)
      {
        frac += '0';
      }
      ns += std::stoll(frac);
    }

    if (match[8].matched)
    {
      int sign = (match[8].str() == "-") ? 1 : -1;
      int tz_hours = std::stoi(match[9].str());
      int tz_mins = std::stoi(match[10].str());
      ns += sign * (tz_hours * kNsPerHour + tz_mins * kNsPerMin);
    }

    return ns;
  }

  static std::string toISO8601(int64_t ns, bool include_nanos = false)
  {
    time_t secs = ns / kNsPerSec;
    int64_t frac_ns = ns % kNsPerSec;

    std::tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");

    if (include_nanos && frac_ns > 0)
    {
      oss << '.' << std::setfill('0') << std::setw(9) << frac_ns;
    }

    oss << 'Z';
    return oss.str();
  }

  static std::string toHumanReadable(int64_t ns)
  {
    time_t secs = ns / kNsPerSec;
    std::tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " UTC";
    return oss.str();
  }

  static std::optional<int64_t> parseDuration(std::string_view str)
  {
    if (str.empty())
    {
      return std::nullopt;
    }

    static const std::regex duration_regex(
        R"((?:(\d+)d)?)"
        R"((?:(\d+)h)?)"
        R"((?:(\d+)m(?!s))?)"
        R"((?:(\d+)s)?)"
        R"((?:(\d+)ms)?)"
        R"((?:(\d+)us)?)"
        R"((?:(\d+)ns)?)");

    std::string input(str);
    std::smatch match;
    if (!std::regex_match(input, match, duration_regex))
    {
      return std::nullopt;
    }

    int64_t ns = 0;
    if (match[1].matched)
    {
      ns += std::stoll(match[1].str()) * kNsPerDay;
    }
    if (match[2].matched)
    {
      ns += std::stoll(match[2].str()) * kNsPerHour;
    }
    if (match[3].matched)
    {
      ns += std::stoll(match[3].str()) * kNsPerMin;
    }
    if (match[4].matched)
    {
      ns += std::stoll(match[4].str()) * kNsPerSec;
    }
    if (match[5].matched)
    {
      ns += std::stoll(match[5].str()) * kNsPerMs;
    }
    if (match[6].matched)
    {
      ns += std::stoll(match[6].str()) * kNsPerUs;
    }
    if (match[7].matched)
    {
      ns += std::stoll(match[7].str());
    }

    return ns > 0 ? std::optional(ns) : std::nullopt;
  }

  static std::string formatDuration(int64_t ns)
  {
    if (ns < 0)
    {
      return "-" + formatDuration(-ns);
    }

    std::ostringstream oss;
    bool any = false;

    auto append = [&](int64_t val, const char* unit)
    {
      if (val > 0)
      {
        oss << val << unit;
        any = true;
      }
    };

    int64_t days = ns / kNsPerDay;
    ns %= kNsPerDay;
    int64_t hours = ns / kNsPerHour;
    ns %= kNsPerHour;
    int64_t mins = ns / kNsPerMin;
    ns %= kNsPerMin;
    int64_t secs = ns / kNsPerSec;
    ns %= kNsPerSec;
    int64_t ms = ns / kNsPerMs;

    append(days, "d");
    append(hours, "h");
    append(mins, "m");
    append(secs, "s");
    if (!any || ms > 0)
    {
      append(ms, "ms");
    }

    return any ? oss.str() : "0ms";
  }

  static constexpr int64_t days(int64_t d) { return d * kNsPerDay; }
  static constexpr int64_t hours(int64_t h) { return h * kNsPerHour; }
  static constexpr int64_t minutes(int64_t m) { return m * kNsPerMin; }
  static constexpr int64_t seconds(int64_t s) { return s * kNsPerSec; }
  static constexpr int64_t milliseconds(int64_t ms) { return ms * kNsPerMs; }
  static constexpr int64_t microseconds(int64_t us) { return us * kNsPerUs; }

  static int64_t nowNs()
  {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
  }

  static int64_t startOfDay(int64_t ns) { return (ns / kNsPerDay) * kNsPerDay; }
  static int64_t endOfDay(int64_t ns) { return startOfDay(ns) + kNsPerDay - 1; }
  static int64_t startOfHour(int64_t ns) { return (ns / kNsPerHour) * kNsPerHour; }

  static std::optional<int64_t> parseRelative(std::string_view str)
  {
    if (str.empty())
    {
      return std::nullopt;
    }
    if (str == "now")
    {
      return nowNs();
    }

    if (str.size() >= 3 && str.substr(0, 3) == "now")
    {
      auto rest = str.substr(3);
      if (rest.empty())
      {
        return nowNs();
      }

      char op = rest[0];
      if (op != '+' && op != '-')
      {
        return std::nullopt;
      }

      auto duration = parseDuration(rest.substr(1));
      if (!duration)
      {
        return std::nullopt;
      }

      return op == '+' ? nowNs() + *duration : nowNs() - *duration;
    }

    return parseISO8601(str);
  }
};

struct TimeRangeEx
{
  int64_t start_ns{0};
  int64_t end_ns{INT64_MAX};

  TimeRangeEx() = default;
  TimeRangeEx(int64_t from, int64_t to) : start_ns(from), end_ns(to) {}

  static TimeRangeEx all() { return TimeRangeEx(0, INT64_MAX); }

  static TimeRangeEx fromTime(int64_t ns) { return TimeRangeEx(ns, INT64_MAX); }
  static TimeRangeEx fromTime(const std::string& iso8601)
  {
    auto ns = TimeUtils::parseISO8601(iso8601);
    return ns ? TimeRangeEx(*ns, INT64_MAX) : all();
  }

  static TimeRangeEx toTime(int64_t ns) { return TimeRangeEx(0, ns); }
  static TimeRangeEx toTime(const std::string& iso8601)
  {
    auto ns = TimeUtils::parseISO8601(iso8601);
    return ns ? TimeRangeEx(0, *ns) : all();
  }

  static TimeRangeEx between(int64_t from, int64_t to) { return TimeRangeEx(from, to); }
  static TimeRangeEx between(const std::string& from, const std::string& to)
  {
    auto from_ns = TimeUtils::parseISO8601(from);
    auto to_ns = TimeUtils::parseISO8601(to);
    return TimeRangeEx(from_ns.value_or(0), to_ns.value_or(INT64_MAX));
  }

  static TimeRangeEx lastDuration(int64_t duration_ns)
  {
    int64_t now = TimeUtils::nowNs();
    return TimeRangeEx(now - duration_ns, now);
  }

  static TimeRangeEx last(const std::string& duration)
  {
    auto ns = TimeUtils::parseDuration(duration);
    return ns ? lastDuration(*ns) : all();
  }

  TimeRangeEx& setStart(int64_t ns)
  {
    start_ns = ns;
    return *this;
  }
  TimeRangeEx& setStart(const std::string& iso8601)
  {
    if (auto ns = TimeUtils::parseISO8601(iso8601))
    {
      start_ns = *ns;
    }
    return *this;
  }

  TimeRangeEx& setEnd(int64_t ns)
  {
    end_ns = ns;
    return *this;
  }
  TimeRangeEx& setEnd(const std::string& iso8601)
  {
    if (auto ns = TimeUtils::parseISO8601(iso8601))
    {
      end_ns = *ns;
    }
    return *this;
  }

  bool contains(int64_t ns) const { return ns >= start_ns && ns <= end_ns; }
  bool overlaps(const TimeRangeEx& other) const
  {
    return start_ns <= other.end_ns && end_ns >= other.start_ns;
  }

  int64_t durationNs() const { return end_ns - start_ns; }
  double durationSeconds() const { return durationNs() / 1e9; }
  double durationHours() const { return durationNs() / (3600.0 * 1e9); }
  double durationDays() const { return durationNs() / (86400.0 * 1e9); }

  bool isValid() const { return start_ns <= end_ns; }
  bool isUnbounded() const { return start_ns == 0 && end_ns == INT64_MAX; }

  std::string toString() const
  {
    return TimeUtils::toISO8601(start_ns) + " to " + TimeUtils::toISO8601(end_ns);
  }
};

namespace literals
{

constexpr int64_t operator""_ns(unsigned long long ns) { return static_cast<int64_t>(ns); }
constexpr int64_t operator""_us(unsigned long long us) { return static_cast<int64_t>(us) * TimeUtils::kNsPerUs; }
constexpr int64_t operator""_ms(unsigned long long ms) { return static_cast<int64_t>(ms) * TimeUtils::kNsPerMs; }
constexpr int64_t operator""_s(unsigned long long s) { return static_cast<int64_t>(s) * TimeUtils::kNsPerSec; }
constexpr int64_t operator""_min(unsigned long long m) { return static_cast<int64_t>(m) * TimeUtils::kNsPerMin; }
constexpr int64_t operator""_h(unsigned long long h) { return static_cast<int64_t>(h) * TimeUtils::kNsPerHour; }
constexpr int64_t operator""_d(unsigned long long d) { return static_cast<int64_t>(d) * TimeUtils::kNsPerDay; }

}  // namespace literals

}  // namespace flox::replay
