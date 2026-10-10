/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/log/log_stream.h"

#include <atomic>

#include "lrvx/log/console_logger.h"

namespace lrvx
{

namespace
{

ConsoleLogger& defaultLogger()
{
  static ConsoleLogger logger;
  return logger;
}

// Optional override. nullptr → use defaultLogger(). Atomic so callers can
// swap loggers from any thread while consumer threads are emitting.
std::atomic<ILogger*> g_logger{nullptr};

}  // namespace

void setGlobalLogger(ILogger* logger)
{
  // The threshold first, then the sink: a line that slips through the macro
  // between the two stores is still filtered by the sink itself, which is
  // where the level is also applied. The reverse order could hand the new
  // sink a level belonging to the old one.
  globalMinLogLevel.store(logger != nullptr ? logger->minLevel() : defaultLogger().minLevel(),
                          std::memory_order_release);
  g_logger.store(logger, std::memory_order_release);
}

LogStream::LogStream(LogLevel level) : _level(level) {}

LogStream::~LogStream()
{
  std::string msg = _stream.str();
  ILogger* logger = g_logger.load(std::memory_order_acquire);
  if (logger == nullptr)
  {
    defaultLogger().log(_level, msg);
    return;
  }
  switch (_level)
  {
    case LogLevel::Info:
      logger->info(msg);
      break;
    case LogLevel::Warn:
      logger->warn(msg);
      break;
    case LogLevel::Error:
      logger->error(msg);
      break;
  }
}

}  // namespace lrvx
