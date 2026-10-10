/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#ifdef LRVX_DISABLE_LOGGING

#define LRVX_LOG_ON() ((void)0)
#define LRVX_LOG_OFF() ((void)0)

#define LRVX_LOG(...) ((void)0)
#define LRVX_LOG_LEVEL(...) ((void)0)
#define LRVX_LOG_INFO(...) ((void)0)
#define LRVX_LOG_WARN(...) ((void)0)
#define LRVX_LOG_ERROR(...) ((void)0)

namespace lrvx
{
inline void enableLogging(bool) {}
inline bool isLoggingEnabled() { return false; }
}  // namespace lrvx

#else

#include <atomic>

#include "lrvx/log/log_stream.h"

namespace lrvx
{

inline std::atomic<bool> loggingEnabled{true};

inline void enableLogging(bool enable) { loggingEnabled.store(enable, std::memory_order_release); }
inline bool isLoggingEnabled() { return loggingEnabled.load(std::memory_order_acquire); }

}  // namespace lrvx

#define LRVX_LOG_ON() ::lrvx::enableLogging(true)
#define LRVX_LOG_OFF() ::lrvx::enableLogging(false)

// The two guards are the whole point of the macro form: neither the
// LogStream (a std::ostringstream) nor the arguments streamed into it are
// touched when the line is not going to be written. The level used to be
// applied inside the sink, after the allocation and after every argument had
// been formatted, so a filtered line cost as much as a written one -- and
// LRVX_LOG_WARN sits on OrderTracker's unknown-order path and in the bus
// consumer loop, where a reconnect burst formats thousands of messages
// nobody will read.
#define LRVX_LOG(...) LRVX_LOG_LEVEL(::lrvx::LogLevel::Info, __VA_ARGS__)
// The level is bound in the if's initializer so that the expression passed as
// `lvl` is evaluated exactly once, whatever the caller writes there -- the
// guard and the LogStream would otherwise both evaluate it.
#define LRVX_LOG_LEVEL(lvl, ...)                                         \
  if (const ::lrvx::LogLevel lrvxLogLevel_ = (lvl);                      \
      !::lrvx::isLoggingEnabled() || lrvxLogLevel_ < ::lrvx::logLevel()) \
    ;                                                                    \
  else                                                                   \
    ::lrvx::LogStream(lrvxLogLevel_) << __VA_ARGS__
#define LRVX_LOG_INFO(...) LRVX_LOG_LEVEL(::lrvx::LogLevel::Info, __VA_ARGS__)
#define LRVX_LOG_WARN(...) LRVX_LOG_LEVEL(::lrvx::LogLevel::Warn, __VA_ARGS__)
#define LRVX_LOG_ERROR(...) LRVX_LOG_LEVEL(::lrvx::LogLevel::Error, __VA_ARGS__)

#endif
