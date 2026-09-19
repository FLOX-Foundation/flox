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
// filesystem through the throwing overloads: fs::create_directories and
// fs::exists. A log directory that could not be made -- an unmounted volume, a
// filled disk, changed permissions -- therefore threw on a thread body, which
// is std::terminate: the logging subsystem killing the thing it exists to
// observe, at rotation, which is to say more likely the busier the process is.
//
// Not being able to write a log is not a reason to stop trading.
//
// The obstruction is a regular file standing where a directory component has
// to be. Neither create_directories nor fopen can work through one, on POSIX
// or on Windows, and it holds from construction onward -- so there is no
// window to race the flush thread for, and nothing has to delete a file the
// logger still has open, which Windows refuses outright.
TEST(AtomicLoggerTest, ARotationThatCannotOpenItsFileDropsTheLogNotTheProcess)
{
  cleanLogs();
  const auto root = getLogDir();
  fs::create_directories(root);

  const fs::path blocker = root / "not-a-directory";
  {
    std::ofstream f(blocker.string());
    f << "x";
  }

  AtomicLoggerOptions opts;
  opts.directory = (blocker / "logs").string();
  opts.basename = "doomed.log";
  opts.maxFileSize = 1;
  opts.rotateInterval = std::chrono::minutes(999);

  // The constructor rotates, and that rotation is the one that cannot succeed.
  AtomicLogger logger(opts);

  logger.error("dropped");
  logger.flush();
  logger.error("dropped as well");
  logger.flush();

  // The drop is counted rather than silent, the process is here to assert it,
  // and flush() returned rather than spinning on a flush thread that a throw
  // would have taken away.
  EXPECT_GT(logger.rotationFailures(), 0u);
  EXPECT_FALSE(fs::exists(opts.directory));
}

// What the test above does NOT cover: the fs::exists in nextArchivePath. That
// one runs only when a file was open and is being renamed away, so reaching it
// requires a directory that goes bad while the logger holds a handle in it --
// which POSIX permits and Windows refuses, and which therefore has no portable
// test. The overload was hardened along with the other, uncovered, rather than
// left throwing because no test could be written for it.
