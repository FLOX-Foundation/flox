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
#include <thread>

using namespace flox;
namespace fs = std::filesystem;

const std::string kLogDir = "/dev/shm/testlogs";

void cleanLogs()
{
  if (fs::exists(kLogDir))
  {
    fs::remove_all(kLogDir);
  }
  fs::create_directories(kLogDir);
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

  AtomicLoggerOptions opts;
  opts.directory = kLogDir;
  opts.basename = "main.log";
  opts.rotateInterval = std::chrono::minutes(999);  // disable time-based rotation
  opts.maxFileSize = 0;                             // disable size-based rotation

  AtomicLogger logger(opts);
  logger.info("hello world");
  logger.warn("warn test");
  logger.error("err test");

  std::this_thread::sleep_for(std::chrono::milliseconds(10));  // flush

  auto lines = readLines(kLogDir + "/main.log");
  ASSERT_EQ(lines.size(), 3);
  EXPECT_TRUE(lines[0].find("INFO") != std::string::npos);
  EXPECT_TRUE(lines[1].find("WARN") != std::string::npos);
  EXPECT_TRUE(lines[2].find("ERROR") != std::string::npos);
}

TEST(AtomicLoggerTest, HonorsLogLevelThreshold)
{
  cleanLogs();

  AtomicLoggerOptions opts;
  opts.directory = kLogDir;
  opts.basename = "threshold.log";
  opts.levelThreshold = LogLevel::Warn;

  AtomicLogger logger(opts);
  logger.info("ignore this");
  logger.warn("this should appear");

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  auto lines = readLines(kLogDir + "/threshold.log");
  ASSERT_EQ(lines.size(), 1);
  EXPECT_TRUE(lines[0].find("WARN") != std::string::npos);
}

TEST(AtomicLoggerTest, RotatesBySize)
{
  cleanLogs();

  AtomicLoggerOptions opts;
  opts.directory = kLogDir;
  opts.basename = "rotating.log";
  opts.maxFileSize = 200;  // force rotation quickly
  opts.rotateInterval = std::chrono::minutes(999);

  AtomicLogger logger(opts);
  for (int i = 0; i < 100; ++i)
  {
    logger.error("line " + std::to_string(i));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  int rotatedCount = 0;
  for (const auto& file : fs::directory_iterator(kLogDir))
  {
    if (file.path().string().find("rotating.log.") != std::string::npos)
    {
      rotatedCount++;
    }
  }

  EXPECT_GT(rotatedCount, 0);
}
