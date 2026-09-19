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

  void flush();

  // Rotations that did not produce a writable file. Nonzero means the log is
  // being dropped: the logger keeps running because a log that cannot be
  // written is not a reason to stop what it was logging.
  uint64_t rotationFailures() const noexcept
  {
    return _rotationFailures.load(std::memory_order_acquire);
  }

 private:
  static constexpr size_t BUFFER_SIZE = 1024;
  static constexpr size_t MAX_MESSAGE_SIZE = 256;

  struct LogEntry
  {
    LogLevel level;
    size_t length;
    char message[MAX_MESSAGE_SIZE];
    std::chrono::system_clock::time_point timestamp;
    // Slot state, as an absolute ring position rather than a flag. For a slot
    // holding position `pos`:
    //   seq == pos      the slot is free and a producer may claim it
    //   seq == pos + 1  the payload is written and the slot is readable
    //   seq == pos + N  retired; this is the free state of the next lap
    // A flag could not express the third state, which is why the old protocol
    // could clear a publication that belonged to the following lap.
    std::atomic<size_t> seq{0};
  };

  AtomicLoggerOptions _opts;
  std::array<LogEntry, BUFFER_SIZE> _buffer{};
  std::atomic<size_t> _writeIndex{0};
  std::atomic<size_t> _readIndex{0};

  // Messages retired (written out or discarded). flush() waits on this
  // rather than on _readIndex, so a message whose slot has been claimed but
  // not yet written out still holds the flush.
  std::atomic<size_t> _retired{0};

  // Ticket pair for handing the fflush to the flush thread. The FILE* belongs
  // to that thread alone: it is the thread that rotates, which closes the
  // handle and opens another one. flush() used to read _file and fflush it
  // from the caller's thread, so a rotation landing in between left the
  // caller flushing a handle that had just been closed. A caller now raises
  // _flushRequest and waits for _flushAcked to catch up.
  std::atomic<uint64_t> _flushRequest{0};
  std::atomic<uint64_t> _flushAcked{0};

  std::atomic<bool> _running{true};
  std::thread _flushThread;
  std::condition_variable _cv;
  std::mutex _cvMutex;

  FILE* _file = nullptr;
  size_t _bytesWritten = 0;
  std::atomic<uint64_t> _rotationFailures{0};
  std::chrono::system_clock::time_point _lastRotation;

  void log(LogLevel level, std::string_view msg);
  void flushLoop();
  // Runs on the flush thread only: fflushes the current file if a caller has
  // asked for it, and publishes the ticket it served.
  void serviceFlushRequest();
  void rotateIfNeeded();
  void rotate();
  std::string nextArchivePath() const;
  // Retires the oldest position in the ring. `emit` writes the payload out;
  // an overwriting producer passes false to discard it. Returns false when
  // there is nothing to retire.
  bool retireOldest(bool emit);
  void writeToOutput(const LogEntry& entry);
  static void formatTimestamp(std::chrono::system_clock::time_point ts, char* buf, size_t bufSize);
};

}  // namespace flox
