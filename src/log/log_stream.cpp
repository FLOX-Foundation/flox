/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/log/log_stream.h"
#include "flox/log/console_logger.h"

namespace flox
{

namespace
{

ConsoleLogger& getConsoleLogger()
{
  static ConsoleLogger logger;
  return logger;
}

}  // namespace

LogStream::LogStream(LogLevel level) : _level(level) {}

LogStream::~LogStream()
{
  getConsoleLogger().log(_level, _stream.str());
}

}  // namespace flox
