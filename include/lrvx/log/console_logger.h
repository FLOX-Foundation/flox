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

#include "lrvx/log/abstract_logger.h"

namespace lrvx
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
  // LRVX_LOG_* checks before formatting anything.
  LogLevel minLevel() const noexcept override { return _minLevel; }

 private:
  LogLevel _minLevel;
};

}  // namespace lrvx
