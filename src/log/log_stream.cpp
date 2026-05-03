/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/log/log_stream.h"

#include <atomic>

#include "flox/log/console_logger.h"

namespace flox
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

}  // namespace flox
