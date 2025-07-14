#include "flox/log/log_stream.h"
#include "flox/log/console_logger.h"

namespace flox
{

namespace
{

ConsoleLogger& getConsoleLogger()
{
  static ConsoleLogger logger;
  return logger;
}

}  // namespace

LogStream::LogStream(LogLevel level) : _level(level) {}

LogStream::~LogStream()
{
  getConsoleLogger().log(_level, _stream.str());
}

}  // namespace flox
