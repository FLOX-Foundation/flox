/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/log/console_logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace flox
{

ConsoleLogger::ConsoleLogger(LogLevel minLevel) : _minLevel(minLevel) {}

void ConsoleLogger::info(std::string_view msg) { log(LogLevel::Info, msg); }
void ConsoleLogger::warn(std::string_view msg) { log(LogLevel::Warn, msg); }
void ConsoleLogger::error(std::string_view msg) { log(LogLevel::Error, msg); }

void ConsoleLogger::log(LogLevel level, std::string_view msg)
{
  if (level < _minLevel)
  {
    return;
  }

  const char* levelStr =
      level == LogLevel::Info ? "INFO" : level == LogLevel::Warn ? "WARN"
                                                                 : "ERROR";

  const char* color =
      level == LogLevel::Info ? "\033[0;37m" : level == LogLevel::Warn ? "\033[0;33m"
                                                                       : "\033[0;31m";

  const char* reset = "\033[0m";

  auto now = std::chrono::system_clock::now();
  auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();

  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm;
  localtime_r(&t, &tm);

  char timestamp[64];
  std::snprintf(timestamp, sizeof(timestamp), "%04d.%02d.%02d-%02d:%02d:%02d.%03lld",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<long long>(ms));

  FILE* out = (level == LogLevel::Error) ? stderr : stdout;
  std::fprintf(out, "%s[%s] %s: %.*s%s\n", color, timestamp, levelStr,
               static_cast<int>(msg.size()), msg.data(), reset);
  std::fflush(out);
}

}  // namespace flox
