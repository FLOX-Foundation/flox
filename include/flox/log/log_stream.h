/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

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