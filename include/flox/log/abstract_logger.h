#pragma once

#include <string_view>

namespace flox
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
  virtual void info(std::string_view msg) = 0;
  virtual void warn(std::string_view msg) = 0;
  virtual void error(std::string_view msg) = 0;
  virtual ~ILogger() = default;
};

}  // namespace flox