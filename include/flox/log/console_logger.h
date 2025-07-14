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
  void info(std::string_view msg);
  void warn(std::string_view msg);
  void error(std::string_view msg);

 private:
  LogLevel _minLevel;
};

}  // namespace flox
