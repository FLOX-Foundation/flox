/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <flox/log/atomic_logger.h>

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

using namespace flox;
namespace fs = std::filesystem;

static fs::path getLogDir()
{
  return fs::temp_directory_path() / "flox_testlogs";
}

void cleanLogs()
{
  auto logDir = getLogDir();
  if (fs::exists(logDir))
  {
    fs::remove_all(logDir);
  }
  fs::create_directories(logDir);
}

std::vector<std::string> readLines(const std::string& path)
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

TEST(AtomicLoggerTest, WritesToFile)
{
  cleanLogs();
  auto logDir = getLogDir();

  {
    AtomicLoggerOptions opts;
    opts.directory = logDir.string();
    opts.basename = "main.log";
    opts.rotateInterval = std::chrono::minutes(999);  // disable time-based rotation
    opts.maxFileSize = 0;                             // disable size-based rotation

    AtomicLogger logger(opts);
    logger.info("hello world");
    logger.warn("warn test");
    logger.error("err test");
    logger.flush();
  }  // logger destructor closes file

  auto lines = readLines((logDir / "main.log").string());
  ASSERT_EQ(lines.size(), 3);
  EXPECT_TRUE(lines[0].find("INFO") != std::string::npos);
  EXPECT_TRUE(lines[1].find("WARN") != std::string::npos);
  EXPECT_TRUE(lines[2].find("ERROR") != std::string::npos);
}

TEST(AtomicLoggerTest, HonorsLogLevelThreshold)
{
  cleanLogs();
  auto logDir = getLogDir();

  {
    AtomicLoggerOptions opts;
    opts.directory = logDir.string();
    opts.basename = "threshold.log";
    opts.levelThreshold = LogLevel::Warn;

    AtomicLogger logger(opts);
    logger.info("ignore this");
    logger.warn("this should appear");
    logger.flush();
  }  // logger destructor closes file

  auto lines = readLines((logDir / "threshold.log").string());
  ASSERT_EQ(lines.size(), 1);
  EXPECT_TRUE(lines[0].find("WARN") != std::string::npos);
}

TEST(AtomicLoggerTest, RotatesBySize)
{
  cleanLogs();
  auto logDir = getLogDir();

  {
    AtomicLoggerOptions opts;
    opts.directory = logDir.string();
    opts.basename = "rotating.log";
    opts.maxFileSize = 200;  // force rotation quickly
    opts.rotateInterval = std::chrono::minutes(999);

    AtomicLogger logger(opts);
    for (int i = 0; i < 100; ++i)
    {
      logger.error("line " + std::to_string(i));
    }
    logger.flush();
  }  // logger destructor closes file

  int rotatedCount = 0;
  for (const auto& file : fs::directory_iterator(logDir))
  {
    if (file.path().string().find("rotating.log.") != std::string::npos)
    {
      rotatedCount++;
    }
  }

  EXPECT_GT(rotatedCount, 0);
}

// A burst larger than the ring is the ordinary shape of a bad minute: a
// reconnect storm, a rejected-order flood. The default policy is free to drop
// once the ring is full. What it is not free to do is stop: the flush thread
// used to move the read index before reading the slot, a producer reused the
// slot underneath it, and the flush thread then cleared a publication of its
// own and spun on that slot forever. The destructor joins that thread, so the
// process could not exit. This test hangs rather than fails when that comes
// back.
TEST(AtomicLoggerTest, DefaultPolicyBurstPastTheRingTerminates)
{
  cleanLogs();
  auto logDir = getLogDir();

  constexpr int kMessages = 20000;

  {
    AtomicLoggerOptions opts;
    opts.directory = logDir.string();
    opts.basename = "burst.log";
    opts.rotateInterval = std::chrono::minutes(999);
    opts.maxFileSize = 0;
    opts.flushImmediately = false;

    AtomicLogger logger(opts);
    for (int i = 0; i < kMessages; ++i)
    {
      logger.info("burst-message-" + std::to_string(i));
    }
    logger.flush();
  }

  auto lines = readLines((logDir / "burst.log").string());
  EXPECT_GT(lines.size(), 0u);
  EXPECT_LE(lines.size(), static_cast<size_t>(kMessages));
  for (const auto& line : lines)
  {
    EXPECT_NE(line.find("burst-message-"), std::string::npos);
  }
}

// Dropping is for a ring that is genuinely full. A consumer that keeps up
// must lose nothing, however far past the ring size the run goes.
TEST(AtomicLoggerTest, NothingIsLostWhileTheConsumerKeepsUp)
{
  cleanLogs();
  auto logDir = getLogDir();

  constexpr int kMessages = 20000;

  {
    AtomicLoggerOptions opts;
    opts.directory = logDir.string();
    opts.basename = "paced.log";
    opts.rotateInterval = std::chrono::minutes(999);
    opts.maxFileSize = 0;
    opts.flushImmediately = false;

    AtomicLogger logger(opts);
    for (int i = 0; i < kMessages; ++i)
    {
      logger.info("paced-message-" + std::to_string(i));
      if ((i % 256) == 255)
      {
        logger.flush();
      }
    }
    logger.flush();
  }

  auto lines = readLines((logDir / "paced.log").string());
  ASSERT_EQ(lines.size(), static_cast<size_t>(kMessages));
  EXPECT_NE(lines.front().find("paced-message-0"), std::string::npos);
  EXPECT_NE(lines.back().find("paced-message-" + std::to_string(kMessages - 1)),
            std::string::npos);
}

// The overwrite policy is allowed to lose the oldest messages. It is not
// allowed to wedge the flush thread, which is what held the destructor -- and
// with it the whole process -- open forever.
TEST(AtomicLoggerTest, OverwritePolicyKeepsDrainingUnderABurst)
{
  cleanLogs();
  auto logDir = getLogDir();

  {
    AtomicLoggerOptions opts;
    opts.directory = logDir.string();
    opts.basename = "overwrite.log";
    opts.rotateInterval = std::chrono::minutes(999);
    opts.maxFileSize = 0;
    opts.overflow = OverflowPolicy::Overwrite;
    opts.flushImmediately = false;

    AtomicLogger logger(opts);
    for (int i = 0; i < 20000; ++i)
    {
      logger.info("overwrite-message-" + std::to_string(i));
    }
    logger.flush();
  }

  auto lines = readLines((logDir / "overwrite.log").string());
  EXPECT_GT(lines.size(), 0u);
  for (const auto& line : lines)
  {
    EXPECT_NE(line.find("overwrite-message-"), std::string::npos);
  }
}

TEST(AtomicLoggerTest, ConcurrentProducersAllReachTheFile)
{
  cleanLogs();
  auto logDir = getLogDir();

  constexpr int kThreads = 4;
  constexpr int kPerThread = 500;

  {
    AtomicLoggerOptions opts;
    opts.directory = logDir.string();
    opts.basename = "threads.log";
    opts.rotateInterval = std::chrono::minutes(999);
    opts.maxFileSize = 0;
    opts.flushImmediately = false;

    AtomicLogger logger(opts);

    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t)
    {
      producers.emplace_back(
          [&logger, t]
          {
            for (int i = 0; i < kPerThread; ++i)
            {
              logger.info("t" + std::to_string(t) + "-m" + std::to_string(i));
              if ((i % 64) == 63)
              {
                logger.flush();
              }
            }
          });
    }
    for (auto& p : producers)
    {
      p.join();
    }
    logger.flush();
  }

  auto lines = readLines((logDir / "threads.log").string());
  EXPECT_EQ(lines.size(), static_cast<size_t>(kThreads * kPerThread));
}

// Rotation names used to carry whole seconds, so a burst that rotated twenty
// times inside one second left one archive behind and the rest went to the
// same name, each rename replacing the last.
TEST(AtomicLoggerTest, FastRotationKeepsEveryArchive)
{
  cleanLogs();
  auto logDir = getLogDir();

  constexpr int kMessages = 40;

  {
    AtomicLoggerOptions opts;
    opts.directory = logDir.string();
    opts.basename = "fast.log";
    opts.maxFileSize = 60;  // roughly one message per file
    opts.rotateInterval = std::chrono::minutes(999);

    AtomicLogger logger(opts);
    for (int i = 0; i < kMessages; ++i)
    {
      logger.info("message-number-" + std::to_string(i) + "-padding-padding");
      logger.flush();
    }
  }

  std::set<std::string> names;
  size_t survived = 0;
  for (const auto& file : fs::directory_iterator(logDir))
  {
    names.insert(file.path().filename().string());
    survived += readLines(file.path().string()).size();
  }

  // Every archive has its own name, so nothing was replaced. Allowing for the
  // rotation that trails the last message, the bulk of the run is on disk.
  EXPECT_GE(names.size(), 20u);
  EXPECT_GE(survived, static_cast<size_t>(kMessages) - 2);
}

// The logger rotates on the flush thread, and rotate() used to reach the
// filesystem through the throwing overloads. A log directory that went away
// under a running process -- an unmounted volume, a filled disk, changed
// permissions -- therefore threw on a thread body, which is std::terminate:
// the logging subsystem killing the thing it exists to observe, at rotation,
// which is to say more likely the busier the process is.
//
// Not being able to write a log is not a reason to stop trading. The logger
// drops output and says so instead.
TEST(AtomicLoggerTest, ARotationThatCannotOpenItsFileDropsTheLogNotTheProcess)
{
  cleanLogs();
  auto logDir = getLogDir();

  AtomicLoggerOptions opts;
  opts.directory = logDir.string();
  opts.basename = "doomed.log";
  opts.maxFileSize = 1;  // every entry rotates
  opts.rotateInterval = std::chrono::minutes(999);

  AtomicLogger logger(opts);
  logger.error("before");
  logger.flush();
  ASSERT_EQ(logger.rotationFailures(), 0u);

  // Put a regular file where the directory was. Neither create_directories
  // nor fopen can succeed past this, and the old code threw on the first.
  fs::remove_all(logDir);
  {
    std::ofstream blocker(logDir.string());
    blocker << "not a directory";
  }

  for (int i = 0; i < 8; ++i)
  {
    logger.error("after " + std::to_string(i));
    logger.flush();
  }

  EXPECT_GT(logger.rotationFailures(), 0u);

  // Still serving: flush() returns, the thread is alive, and the process is
  // here to assert it.
  logger.error("still alive");
  logger.flush();

  fs::remove(logDir);
}
