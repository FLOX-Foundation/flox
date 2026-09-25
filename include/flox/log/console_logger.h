/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <string_view>

#include "flox/log/abstract_logger.h"

namespace flox
{

class ConsoleLogger final : public ILogger
{
 public:
  explicit ConsoleLogger(LogLevel minLevel = LogLevel::Info);

  void log(LogLevel level, std::string_view msg);
  void info(std::string_view msg) override;
  void warn(std::string_view msg) override;
  void error(std::string_view msg) override;

  // The threshold the logger was constructed with, which is also what
  // FLOX_LOG_* checks before formatting anything.
  LogLevel minLevel() const noexcept override { return _minLevel; }

 private:
  LogLevel _minLevel;
};

}  // namespace flox
