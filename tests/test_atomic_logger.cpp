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
