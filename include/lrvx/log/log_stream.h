/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <atomic>
#include <sstream>

#include "lrvx/log/abstract_logger.h"

namespace lrvx
{

// Replace the global logger for the rest of this process. NULL resets to
// the default `ConsoleLogger`. The pointer is non-owning; the caller owns
// the lifetime of the supplied ILogger and must outlive any concurrent
// LRVX_LOG_* call. Atomic acquire/release: safe to swap with consumer
// threads active.
//
// Also republishes the sink's minLevel() into logLevel() below, so the
// threshold is one relaxed load away from the macro instead of a pointer
// load and a virtual call per log line.
void setGlobalLogger(ILogger* logger);

// The level the installed sink accepts, kept beside the sink itself. Lines
// below it are not formatted at all: LRVX_LOG_* checks this before it builds
// a LogStream. Info until a sink with a higher threshold is installed, which
// is what the default ConsoleLogger accepts.
inline std::atomic<LogLevel> globalMinLogLevel{LogLevel::Info};

inline LogLevel logLevel() noexcept
{
  return globalMinLogLevel.load(std::memory_order_acquire);
}

class LogStream
{
 public:
  explicit LogStream(LogLevel level = LogLevel::Info);
  ~LogStream();

  template <typename T>
  LogStream& operator<<(const T& val)
  {
    _stream << val;
    return *this;
  }

 private:
  LogLevel _level;
  std::ostringstream _stream;
};

}  // namespace lrvx