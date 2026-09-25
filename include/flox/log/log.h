/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#ifdef FLOX_DISABLE_LOGGING

#define FLOX_LOG_ON() ((void)0)
#define FLOX_LOG_OFF() ((void)0)

#define FLOX_LOG(...) ((void)0)
#define FLOX_LOG_LEVEL(...) ((void)0)
#define FLOX_LOG_INFO(...) ((void)0)
#define FLOX_LOG_WARN(...) ((void)0)
#define FLOX_LOG_ERROR(...) ((void)0)

namespace flox
{
inline void enableLogging(bool) {}
inline bool isLoggingEnabled() { return false; }
}  // namespace flox

#else

#include <atomic>

#include "flox/log/log_stream.h"

namespace flox
{

inline std::atomic<bool> loggingEnabled{true};

inline void enableLogging(bool enable) { loggingEnabled.store(enable, std::memory_order_release); }
inline bool isLoggingEnabled() { return loggingEnabled.load(std::memory_order_acquire); }

}  // namespace flox

#define FLOX_LOG_ON() ::flox::enableLogging(true)
#define FLOX_LOG_OFF() ::flox::enableLogging(false)

// The two guards are the whole point of the macro form: neither the
// LogStream (a std::ostringstream) nor the arguments streamed into it are
// touched when the line is not going to be written. The level used to be
// applied inside the sink, after the allocation and after every argument had
// been formatted, so a filtered line cost as much as a written one -- and
// FLOX_LOG_WARN sits on OrderTracker's unknown-order path and in the bus
// consumer loop, where a reconnect burst formats thousands of messages
// nobody will read.
#define FLOX_LOG(...) FLOX_LOG_LEVEL(::flox::LogLevel::Info, __VA_ARGS__)
// The level is bound in the if's initializer so that the expression passed as
// `lvl` is evaluated exactly once, whatever the caller writes there -- the
// guard and the LogStream would otherwise both evaluate it.
#define FLOX_LOG_LEVEL(lvl, ...)                                         \
  if (const ::flox::LogLevel floxLogLevel_ = (lvl);                      \
      !::flox::isLoggingEnabled() || floxLogLevel_ < ::flox::logLevel()) \
    ;                                                                    \
  else                                                                   \
    ::flox::LogStream(floxLogLevel_) << __VA_ARGS__
#define FLOX_LOG_INFO(...) FLOX_LOG_LEVEL(::flox::LogLevel::Info, __VA_ARGS__)
#define FLOX_LOG_WARN(...) FLOX_LOG_LEVEL(::flox::LogLevel::Warn, __VA_ARGS__)
#define FLOX_LOG_ERROR(...) FLOX_LOG_LEVEL(::flox::LogLevel::Error, __VA_ARGS__)

#endif
