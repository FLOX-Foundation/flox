/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/log/atomic_logger.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>

namespace flox
{

namespace fs = std::filesystem;

AtomicLogger::AtomicLogger(AtomicLoggerOptions opts)
    : _opts(std::move(opts)),
      _bytesWritten(0),
      _lastRotation(std::chrono::system_clock::now())
{
  rotate();
  _flushThread = std::thread(&AtomicLogger::flushLoop, this);
}

AtomicLogger::~AtomicLogger()
{
  _running = false;
  _cv.notify_one();
  if (_flushThread.joinable())
    _flushThread.join();
  if (_file)
    std::fclose(_file);
}

void AtomicLogger::info(std::string_view msg) { log(LogLevel::Info, msg); }
void AtomicLogger::warn(std::string_view msg) { log(LogLevel::Warn, msg); }
void AtomicLogger::error(std::string_view msg) { log(LogLevel::Error, msg); }

void AtomicLogger::log(LogLevel level, std::string_view msg)
{
  if (level < _opts.levelThreshold)
    return;

  size_t currentWrite = _writeIndex.load(std::memory_order_relaxed);
  size_t currentRead = _readIndex.load(std::memory_order_acquire);

  if (currentWrite - currentRead >= BUFFER_SIZE)
  {
    if (_opts.overflow == OverflowPolicy::Drop)
      return;
    _readIndex.fetch_add(1, std::memory_order_relaxed);
  }

  const size_t idx = _writeIndex.fetch_add(1, std::memory_order_acq_rel) % BUFFER_SIZE;
  auto& entry = _buffer[idx];

  entry.level = level;
  entry.timestamp = std::chrono::system_clock::now();
  entry.length = std::min(msg.size(), MAX_MESSAGE_SIZE - 1);
  std::memcpy(entry.message, msg.data(), entry.length);
  entry.message[entry.length] = '\0';

  _cv.notify_one();
}

void AtomicLogger::flushLoop()
{
  while (_running)
  {
    {
      std::unique_lock lock(_cvMutex);
      _cv.wait_for(lock, std::chrono::milliseconds(1));
    }

    rotateIfNeeded();

    while (_readIndex.load(std::memory_order_acquire) < _writeIndex.load(std::memory_order_acquire))
    {
      const size_t idx = _readIndex.fetch_add(1, std::memory_order_acq_rel) % BUFFER_SIZE;
      writeToOutput(_buffer[idx]);
    }
  }

  while (_readIndex.load() < _writeIndex.load())
  {
    const size_t idx = _readIndex.fetch_add(1) % BUFFER_SIZE;
    writeToOutput(_buffer[idx]);
  }
}

void AtomicLogger::rotateIfNeeded()
{
  auto now = std::chrono::system_clock::now();

  if (_opts.maxFileSize > 0 && _bytesWritten > _opts.maxFileSize)
    rotate();

  if (_opts.rotateInterval.count() > 0 && now - _lastRotation >= _opts.rotateInterval)
    rotate();
}

void AtomicLogger::rotate()
{
  if (_file)
  {
    char timestamp[32];
    formatTimestamp(_lastRotation, timestamp, sizeof(timestamp));

    std::ostringstream newName;
    newName << _opts.directory << "/" << _opts.basename << "." << timestamp << ".log";

    std::string oldPath = _opts.directory + "/" + _opts.basename;
    std::rename(oldPath.c_str(), newName.str().c_str());
    std::fclose(_file);
  }

  std::string path = _opts.directory + "/" + _opts.basename;
  fs::create_directories(_opts.directory);
  _file = std::fopen(path.c_str(), "w");
  _bytesWritten = 0;
  _lastRotation = std::chrono::system_clock::now();
}

void AtomicLogger::writeToOutput(const LogEntry& entry)
{
  if (!_file)
    return;

  const char* levelStr =
      entry.level == LogLevel::Info ? "INFO" : entry.level == LogLevel::Warn ? "WARN"
                                                                             : "ERROR";

  char timebuf[32];
  formatTimestamp(entry.timestamp, timebuf, sizeof(timebuf));

  int written = std::fprintf(_file, "[%s] %s: %s\n", timebuf, levelStr, entry.message);
  if (written > 0)
    _bytesWritten += written;

  if (_opts.flushImmediately)
    std::fflush(_file);
}

void AtomicLogger::formatTimestamp(std::chrono::system_clock::time_point ts, char* buf, size_t bufSize)
{
  using namespace std::chrono;
  auto in_time_t = system_clock::to_time_t(ts);
  auto ms = duration_cast<milliseconds>(ts.time_since_epoch()) % 1000;

  std::tm tm;
  localtime_r(&in_time_t, &tm);
  std::snprintf(buf, bufSize, "%04d%02d%02d-%02d%02d%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
}

}  // namespace flox