/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Where the default log goes, and what a filtered log line costs.
//
// AtomicLoggerOptions::directory defaults to "/dev/shm", which exists on
// Linux and on neither of the other two platforms the build matrix covers.
// There, create_directories fails, the file pointer stays null, and
// writeToOutput drops every line for the life of the process after a single
// line on stderr. A default that silently discards the log on two platforms
// is not a default.
//
// FLOX_LOG_* is the second half. The macros guard on the global on/off flag
// and then build a LogStream -- a std::ostringstream -- and format the whole
// message into it. The level is compared against the sink's threshold only
// after that, inside the sink, so a filtered line still costs the allocation
// and the formatting of every argument. FLOX_LOG_WARN sits on OrderTracker's
// unknown-order path and in the bus consumer loop, so a reconnect burst
// formats thousands of messages nobody will read.

#include <flox/log/abstract_logger.h>
#include <flox/log/atomic_logger.h>
#include <flox/log/console_logger.h>
#include <flox/log/log.h>
#include <flox/log/log_stream.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace flox;
namespace fs = std::filesystem;

namespace
{

int g_evaluations = 0;

// Stands in for whatever a real log argument costs -- a price formatted, a
// symbol looked up, a snapshot summarised. It is counted rather than timed so
// the test says what happened rather than how fast.
int evaluatedArgument()
{
  ++g_evaluations;
  return g_evaluations;
}

// A sink that accepts nothing below Error and records what reached it.
class ErrorOnlySink final : public ILogger
{
 public:
  void info(std::string_view) override { ++filtered; }
  void warn(std::string_view) override { ++filtered; }
  void error(std::string_view msg) override
  {
    ++accepted;
    last.assign(msg);
  }

  int filtered{0};
  int accepted{0};
  std::string last;
};

class GlobalSinkGuard
{
 public:
  explicit GlobalSinkGuard(ILogger* sink) { setGlobalLogger(sink); }
  ~GlobalSinkGuard() { setGlobalLogger(nullptr); }
};

std::vector<std::string> readLines(const fs::path& path)
{
  std::ifstream f(path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line))
  {
    lines.push_back(line);
  }
  return lines;
}

}  // namespace

// The green control for the logger: told where to write, it writes there.
TEST(AtomicLoggerDefaults, AnExplicitDirectoryReceivesEveryLine)
{
  const fs::path dir = fs::temp_directory_path() / "flox_default_dir_probe";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  ASSERT_FALSE(ec);

  {
    AtomicLoggerOptions opts;
    opts.directory = dir.string();
    opts.basename = "probe.log";
    opts.rotateInterval = std::chrono::minutes(999);
    opts.maxFileSize = 0;

    AtomicLogger logger(opts);
    EXPECT_EQ(logger.rotationFailures(), 0u);
    logger.info("one");
    logger.error("two");
    logger.flush();
  }

  EXPECT_EQ(readLines(dir / "probe.log").size(), 2u);
  fs::remove_all(dir, ec);
}

// The default is what a caller gets who has not thought about it yet -- the
// demo, the first strategy, the bug report that arrives with no log attached.
// It must not be a directory this host does not have.
TEST(AtomicLoggerDefaults, TheDefaultDirectoryIsOneTheHostActuallyHas)
{
  const AtomicLoggerOptions defaults;

  AtomicLogger logger;  // defaults, exactly as a first-time caller gets them
  logger.info("a line that must not vanish");
  logger.error("nor this one");
  logger.flush();

  EXPECT_EQ(logger.rotationFailures(), 0u)
      << "a default-constructed AtomicLogger could not open '" << defaults.directory
      << "/" << defaults.basename
      << "' on this host, so every line it is given is dropped. The default "
         "has to be a directory that exists here (a fall back to the system "
         "temp directory is enough) or the constructor has to refuse loudly "
         "rather than run on with no file.";
}

// The green control for the macros: the existing on/off guard already keeps
// the argument from being evaluated, so the mechanism is available and the
// failure below is about the level, not about the macro shape.
TEST(LogCost, DisabledLoggingDoesNotEvaluateTheMessage)
{
  ErrorOnlySink sink;
  GlobalSinkGuard guard(&sink);

  g_evaluations = 0;
  FLOX_LOG_OFF();
  FLOX_LOG_INFO("value=" << evaluatedArgument());
  FLOX_LOG_WARN("value=" << evaluatedArgument());
  FLOX_LOG_ON();

  EXPECT_EQ(g_evaluations, 0);
  EXPECT_EQ(sink.filtered, 0);
}

// Raising the threshold is done by installing a sink that carries one --
// ConsoleLogger takes its minimum level in the constructor, and that is the
// only threshold the tree has. Whatever the level ends up being read through,
// FLOX_LOG_LEVEL has to consult it before it builds the stream.
//
// needs, either of:
//   virtual LogLevel ILogger::minLevel() const noexcept;  consulted by
//     FLOX_LOG_LEVEL through the installed sink, with ConsoleLogger and
//     AtomicLogger returning the threshold they were built with;
//   or a process-global LogLevel that setGlobalLogger records from the sink
//     and the macro reads (flox::logLevel()).
TEST(LogCost, AFilteredLevelDoesNotEvaluateTheMessage)
{
  ConsoleLogger errorsOnly(LogLevel::Error);  // drops Info and Warn
  GlobalSinkGuard guard(&errorsOnly);

  g_evaluations = 0;
  for (int i = 0; i < 100; ++i)
  {
    FLOX_LOG_INFO("value=" << evaluatedArgument());
    FLOX_LOG_WARN("value=" << evaluatedArgument());
  }

  EXPECT_EQ(g_evaluations, 0)
      << "the arguments of 200 filtered log lines were evaluated " << g_evaluations
      << " times, and each line also built and formatted a std::ostringstream, "
         "before the sink decided it wanted none of them";
}

// And the other half: a level the sink does accept must still be built and
// delivered in full. A fix that simply stops evaluating is not a fix.
TEST(LogCost, AnAcceptedLevelIsStillFormattedAndDelivered)
{
  ErrorOnlySink sink;
  GlobalSinkGuard guard(&sink);

  g_evaluations = 0;
  FLOX_LOG_ERROR("value=" << evaluatedArgument());

  EXPECT_EQ(g_evaluations, 1);
  EXPECT_EQ(sink.accepted, 1);
  EXPECT_NE(sink.last.find("value=1"), std::string::npos)
      << "the accepted line reached the sink as '" << sink.last << "'";
}
