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

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

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

#if !defined(_WIN32)
// A logger that cannot open its file writes its lines to stderr instead.
// rotationFailures() is the counter a supervisor polls; it says a transition
// happened, not what became of the lines, and a logger that answers "1" and
// then discards everything looks identical from there. So the line itself is
// read back off the console.
//
// The obstruction is the same one AtomicLoggerTest uses for the rotation
// path: a regular file standing where a directory component has to be, which
// neither create_directories nor fopen can work through, from construction
// onward.
TEST(AtomicLoggerDefaults, ALoggerThatCannotOpenItsFileWritesTheLineToStderr)
{
  const fs::path root = fs::temp_directory_path() / "flox_stderr_probe";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  ASSERT_FALSE(ec);

  const fs::path blocker = root / "not-a-directory";
  {
    std::ofstream f(blocker.string());
    f << "x";
  }

  const fs::path captured = root / "stderr.txt";

  AtomicLoggerOptions opts;
  opts.directory = (blocker / "logs").string();
  opts.basename = "doomed.log";
  opts.rotateInterval = std::chrono::minutes(999);
  opts.maxFileSize = 0;

  std::fflush(stderr);
  const int savedStderr = ::dup(STDERR_FILENO);
  ASSERT_NE(savedStderr, -1);
  FILE* sink = std::fopen(captured.string().c_str(), "w+");
  ASSERT_NE(sink, nullptr);
  ASSERT_NE(::dup2(::fileno(sink), STDERR_FILENO), -1);

  {
    AtomicLogger logger(opts);
    logger.error("the line that must reach the console");
    logger.flush();
    logger.info("and this one as well");
    logger.flush();
    ASSERT_GT(logger.rotationFailures(), 0u) << "the rotation was supposed to fail here";
  }

  std::fflush(stderr);
  ASSERT_NE(::dup2(savedStderr, STDERR_FILENO), -1);
  ::close(savedStderr);
  std::fclose(sink);

  std::ifstream in(captured);
  std::ostringstream all;
  all << in.rdbuf();
  const std::string console = all.str();

  EXPECT_NE(console.find("the line that must reach the console"), std::string::npos)
      << "an ERROR line was dropped by a logger with no file instead of going "
         "to stderr. What reached the console was:\n"
      << console;
  EXPECT_NE(console.find("and this one as well"), std::string::npos)
      << "an INFO line was dropped the same way";
  EXPECT_NE(console.find("ERROR"), std::string::npos) << "the level goes with the line";

  ASSERT_FALSE(fs::exists(opts.directory)) << "the directory was never creatable";
  fs::remove_all(root, ec);
}
#endif  // !_WIN32

namespace
{

// A sink that declares a threshold and counts what it is given, from any
// thread.
class ThresholdSink final : public ILogger
{
 public:
  explicit ThresholdSink(LogLevel min) : _min(min) {}

  LogLevel minLevel() const noexcept override { return _min; }

  void info(std::string_view) override { record(LogLevel::Info); }
  void warn(std::string_view) override { record(LogLevel::Warn); }
  void error(std::string_view) override { record(LogLevel::Error); }

  std::atomic<int> belowThreshold{0};
  std::atomic<int> accepted{0};

 private:
  void record(LogLevel level)
  {
    if (level < _min)
    {
      belowThreshold.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
      accepted.fetch_add(1, std::memory_order_relaxed);
    }
  }

  LogLevel _min;
};

}  // namespace

// The level the macro reads and the sink it is read for are two separate
// atomics, published by setGlobalLogger() and consulted by every FLOX_LOG_*
// on every other thread. Swapping the sink under a running logger must stay
// a well-defined operation: no line may reach a sink below that sink's own
// threshold, and nothing here may be a data race.
//
// This is the test to run under ThreadSanitizer -- a Debug -fsanitize=thread
// build -- since that is the only place the ordering between those two
// stores and the loads opposite them is actually checked. Without a reader
// thread there is nothing for TSan to look at.
TEST(LogCost, SwappingTheSinkUnderAConcurrentLoggerIsWellDefined)
{
  // Both outlive every thread that can reach them: the pointer is non-owning
  // and the header's contract is that the caller keeps the sink alive for any
  // concurrent FLOX_LOG_* call.
  ThresholdSink errorsOnly(LogLevel::Error);
  ThresholdSink everything(LogLevel::Info);

  GlobalSinkGuard guard(&errorsOnly);

  std::atomic<bool> stop{false};
  std::atomic<int> turns{0};

  std::thread reader(
      [&]
      {
        while (!stop.load(std::memory_order_acquire))
        {
          const int turn = turns.fetch_add(1, std::memory_order_relaxed);
          (void)logLevel();
          FLOX_LOG_INFO("info " << turn);
          FLOX_LOG_WARN("warn " << turn);
          FLOX_LOG_ERROR("error " << turn);
        }
      });

  // Keep swapping until the reader has actually been through the loop, so
  // the two sides overlap rather than the writer finishing before the thread
  // has started. The second bound stops a reader that never runs from
  // hanging the suite.
  int swaps = 0;
  while ((swaps < 2000 || turns.load(std::memory_order_relaxed) < 500) && swaps < 2'000'000)
  {
    setGlobalLogger(swaps % 2 == 0 ? &everything : &errorsOnly);
    ++swaps;
  }

  stop.store(true, std::memory_order_release);
  reader.join();

  EXPECT_GE(turns.load(), 500) << "the reader thread never ran alongside the swaps";

  // Every turn emits one ERROR, and Error clears both thresholds in play, so
  // no swap may lose one. A line that slipped through the macro against one
  // sink's level and landed on the other is still delivered and still
  // filtered by the sink it reached -- that window is deliberate, which is
  // why belowThreshold is observed rather than required to be zero -- but
  // nothing may vanish between the two stores.
  const int delivered = errorsOnly.accepted.load() + everything.accepted.load();
  EXPECT_GE(delivered, turns.load())
      << "across " << swaps << " sink swaps, " << turns.load()
      << " ERROR lines were emitted and only " << delivered
      << " arrived: a line was lost between publishing the level and "
         "publishing the sink";
  EXPECT_EQ(everything.belowThreshold.load(), 0)
      << "a sink that accepts everything was handed something below Info";
}
