#pragma once

#include <sstream>

#include "flox/log/abstract_logger.h"

namespace flox
{

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

}  // namespace flox