/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "flox/log/abstract_logger.h"

namespace flox
{

struct AtomicLoggerOptions
{
  OverflowPolicy overflow = OverflowPolicy::Drop;
  LogLevel levelThreshold = LogLevel::Info;

  std::string basename = "flox.log";
  std::string directory = "/dev/shm";
  size_t maxFileSize = 100 * 1024 * 1024;
  std::chrono::minutes rotateInterval = std::chrono::minutes(60);

  bool flushImmediately = true;
};

class AtomicLogger final : public ILogger
{
 public:
  explicit AtomicLogger(AtomicLoggerOptions opts = {});
  ~AtomicLogger();

  void info(std::string_view msg) override;
  void warn(std::string_view msg) override;
  void error(std::string_view msg) override;

 private:
  static constexpr size_t BUFFER_SIZE = 1024;
  static constexpr size_t MAX_MESSAGE_SIZE = 256;

  struct LogEntry
  {
    LogLevel level;
    size_t length;
    char message[MAX_MESSAGE_SIZE];
    std::chrono::system_clock::time_point timestamp;
  };

  AtomicLoggerOptions _opts;
  std::array<LogEntry, BUFFER_SIZE> _buffer{};
  std::atomic<size_t> _writeIndex{0};
  std::atomic<size_t> _readIndex{0};

  std::atomic<bool> _running{true};
  std::thread _flushThread;
  std::condition_variable _cv;
  std::mutex _cvMutex;

  FILE* _file = nullptr;
  size_t _bytesWritten = 0;
  std::chrono::system_clock::time_point _lastRotation;

  void log(LogLevel level, std::string_view msg);
  void flushLoop();
  void rotateIfNeeded();
  void rotate();
  void writeToOutput(const LogEntry& entry);
  void formatTimestamp(std::chrono::system_clock::time_point ts, char* buf, size_t bufSize);
};

}  // namespace flox
