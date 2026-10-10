/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <string_view>

namespace lrvx
{

enum class LogLevel
{
  Info,
  Warn,
  Error
};

enum class OverflowPolicy
{
  Drop,      // silently drop new messages if buffer full
  Overwrite  // overwrite oldest messages
};

struct ILogger
{
  virtual ~ILogger() = default;

  virtual void info(std::string_view msg) = 0;
  virtual void warn(std::string_view msg) = 0;
  virtual void error(std::string_view msg) = 0;

  // The lowest level this sink accepts. LRVX_LOG_* reads it through the
  // installed logger and stops before it builds the stream, so a line the
  // sink is going to throw away costs neither the ostringstream nor the
  // evaluation of its arguments. The threshold used to be applied inside the
  // sink only, after the allocation and the formatting -- which put an
  // allocation storm on the execution path every time a reconnect burst hit
  // the unknown-order warning.
  //
  // Info by default: a sink that does not declare a threshold keeps receiving
  // everything, exactly as before.
  virtual LogLevel minLevel() const noexcept { return LogLevel::Info; }
};

}  // namespace lrvx