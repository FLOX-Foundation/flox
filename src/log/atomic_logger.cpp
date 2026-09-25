/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/log/atomic_logger.h"
#include "flox/util/concurrency/thread_body.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>

namespace flox
{

namespace fs = std::filesystem;

const std::string& defaultLogDirectory()
{
  // Resolved once: the answer cannot change under a running process, and a
  // stat per default-constructed options object would be paid on a path that
  // is often taken in a constructor.
  static const std::string dir = []
  {
    std::error_code ec;
    const fs::path shm{"/dev/shm"};
    if (fs::is_directory(shm, ec))
    {
      return shm.string();
    }
    const fs::path tmp = fs::temp_directory_path(ec);
    if (!ec && !tmp.empty())
    {
      return tmp.string();
    }
    // Neither answered: the current directory always exists, and a relative
    // path is still a file somebody can find. The rotation counter reports it
    // if even that cannot be opened.
    return std::string(".");
  }();
  return dir;
}

namespace
{
// A producer gives up after this many turns around the claim loop. Each turn
// either claims a slot, frees one, or loses a race to another producer, so
// the cap only matters when several producers are fighting over the last
// slot; it keeps logging from becoming an unbounded spin.
constexpr size_t kClaimAttempts = 64;
}  // namespace

AtomicLogger::AtomicLogger(AtomicLoggerOptions opts)
    : _opts(std::move(opts)),
      _bytesWritten(0),
      _lastRotation(std::chrono::system_clock::now())
{
  for (size_t i = 0; i < BUFFER_SIZE; ++i)
  {
    _buffer[i].seq.store(i, std::memory_order_relaxed);
  }

  rotate();
  _flushThread = makeThread(
      "flox.log.flush", [this]
      { flushLoop(); },
      [this]
      {
        // flush() spins until the flush thread acknowledges its ticket. With
        // the thread gone that spin never ends, so dropping _running takes the
        // same exit the destructor uses: every waiter observes it and leaves.
        _running.store(false, std::memory_order_release);
        _cv.notify_all();
      });
}

AtomicLogger::~AtomicLogger()
{
  _running = false;
  _cv.notify_one();
  if (_flushThread.joinable())
  {
    _flushThread.join();
  }
  if (_file)
  {
    std::fclose(_file);
  }
}

void AtomicLogger::info(std::string_view msg) { log(LogLevel::Info, msg); }
void AtomicLogger::warn(std::string_view msg) { log(LogLevel::Warn, msg); }
void AtomicLogger::error(std::string_view msg) { log(LogLevel::Error, msg); }

// Waits for everything claimed before this call to reach the file, then has
// the flush thread fflush it. The caller never touches the FILE* itself: the
// flush thread owns it, and it is the thread that closes and reopens the
// handle on rotation.
void AtomicLogger::flush()
{
  _cv.notify_one();

  while (_running.load(std::memory_order_acquire) &&
         _retired.load(std::memory_order_acquire) < _writeIndex.load(std::memory_order_acquire))
  {
    std::this_thread::yield();
  }

  // Raised after the drain, so the ticket is served once the payloads are
  // already in the stream.
  const uint64_t ticket = _flushRequest.fetch_add(1, std::memory_order_acq_rel) + 1;
  _cv.notify_one();

  while (_running.load(std::memory_order_acquire) &&
         _flushAcked.load(std::memory_order_acquire) < ticket)
  {
    std::this_thread::yield();
  }
  // A logger being torn down stops waiting here: the destructor joins the
  // flush thread and closes the file, which flushes it.
}

void AtomicLogger::serviceFlushRequest()
{
  const uint64_t requested = _flushRequest.load(std::memory_order_acquire);
  if (requested == _flushAcked.load(std::memory_order_relaxed))
  {
    return;
  }

  if (_file)
  {
    std::fflush(_file);
  }

  _flushAcked.store(requested, std::memory_order_release);
}

// Claim a slot, fill it, publish it. A slot is claimable only while its seq
// equals the position being claimed, which is what keeps a producer off a
// slot the flush thread has not finished with. The previous protocol let the
// flush thread advance the read index before reading the slot, so a producer
// could take the slot out from under it, and the flush thread then cleared a
// publication that belonged to the next lap and spun on it forever.
void AtomicLogger::log(LogLevel level, std::string_view msg)
{
  if (level < _opts.levelThreshold)
  {
    return;
  }

  size_t pos = _writeIndex.load(std::memory_order_relaxed);
  LogEntry* entry = nullptr;

  for (size_t attempt = 0; attempt < kClaimAttempts; ++attempt)
  {
    LogEntry& candidate = _buffer[pos % BUFFER_SIZE];
    const size_t seq = candidate.seq.load(std::memory_order_acquire);
    const std::ptrdiff_t diff =
        static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);

    if (diff == 0)
    {
      if (_writeIndex.compare_exchange_weak(pos, pos + 1,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed))
      {
        entry = &candidate;
        break;
      }
      continue;
    }

    if (diff < 0)
    {
      // Full: the slot still holds a message nobody has written out.
      if (_opts.overflow == OverflowPolicy::Drop)
      {
        return;
      }

      // Overwrite: retire the oldest message to make room. retireOldest
      // refuses a slot whose payload is still being written, in which case
      // this message is the one that goes.
      if (!retireOldest(false))
      {
        return;
      }

      pos = _writeIndex.load(std::memory_order_relaxed);
      continue;
    }

    // Another producer already took this position.
    pos = _writeIndex.load(std::memory_order_relaxed);
  }

  if (entry == nullptr)
  {
    return;
  }

  entry->level = level;
  entry->timestamp = std::chrono::system_clock::now();
  entry->length = std::min(msg.size(), MAX_MESSAGE_SIZE - 1);
  std::memcpy(entry->message, msg.data(), entry->length);
  entry->message[entry->length] = '\0';

  // Publish only now: everything above must be visible to the consumer before
  // it is allowed to read this slot.
  entry->seq.store(pos + 1, std::memory_order_release);

  _cv.notify_one();
}

// Take the oldest position out of the ring. Winning the read-index exchange
// makes the caller the sole owner of that slot, and the slot is handed back
// to the producers only afterwards, by moving its seq a full lap forward.
bool AtomicLogger::retireOldest(bool emit)
{
  size_t pos = _readIndex.load(std::memory_order_relaxed);

  for (;;)
  {
    if (pos >= _writeIndex.load(std::memory_order_acquire))
    {
      return false;
    }

    LogEntry& entry = _buffer[pos % BUFFER_SIZE];
    if (entry.seq.load(std::memory_order_acquire) != pos + 1)
    {
      // Claimed, payload still being written. The flush thread waits for it;
      // a producer clearing space must not, or it would free a slot out from
      // under the producer filling it.
      if (!emit)
      {
        return false;
      }
      std::this_thread::yield();
      pos = _readIndex.load(std::memory_order_relaxed);
      continue;
    }

    if (!_readIndex.compare_exchange_weak(pos, pos + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed))
    {
      continue;
    }

    if (emit)
    {
      writeToOutput(entry);
    }

    entry.seq.store(pos + BUFFER_SIZE, std::memory_order_release);
    _retired.fetch_add(1, std::memory_order_release);
    return true;
  }
}

void AtomicLogger::flushLoop()
{
  while (_running)
  {
    {
      std::unique_lock lock(_cvMutex);
      _cv.wait_for(lock, std::chrono::milliseconds(1));
    }

    while (_readIndex.load(std::memory_order_acquire) < _writeIndex.load(std::memory_order_acquire))
    {
      rotateIfNeeded();
      if (!retireOldest(true))
      {
        break;
      }
    }

    serviceFlushRequest();
  }

  while (_readIndex.load(std::memory_order_acquire) < _writeIndex.load(std::memory_order_acquire))
  {
    rotateIfNeeded();
    if (!retireOldest(true))
    {
      break;
    }
  }

  // Serve whatever was outstanding before the thread goes away, so a caller
  // parked in flush() during shutdown is released rather than left spinning.
  serviceFlushRequest();
}

void AtomicLogger::rotateIfNeeded()
{
  auto now = std::chrono::system_clock::now();

  if (_opts.maxFileSize > 0 && _bytesWritten > _opts.maxFileSize)
  {
    rotate();
  }

  if (_opts.rotateInterval.count() > 0 && now - _lastRotation >= _opts.rotateInterval)
  {
    rotate();
  }
}

// Name for the archive the current file is about to become. Size-based
// rotation fires many times a second on a busy logger, and the name used to
// carry whole seconds only, so every rename after the first silently replaced
// the archive before it. The milliseconds were already being computed and
// dropped; they are part of the name now, and a suffix settles whatever still
// collides.
std::string AtomicLogger::nextArchivePath() const
{
  char timestamp[32];
  formatTimestamp(_lastRotation, timestamp, sizeof(timestamp));

  std::ostringstream base;
  base << _opts.directory << "/" << _opts.basename << "." << timestamp;

  std::string candidate = base.str() + ".log";
  // The error_code overload, not the throwing one: this runs on the flush
  // thread, and an unreadable directory must not be able to take the process
  // down through the logger. A query that fails answers "does not exist",
  // which at worst reuses a name -- the right trade against terminating.
  std::error_code ec;
  for (int dup = 1; dup < 10000 && fs::exists(candidate, ec); ++dup)
  {
    candidate = base.str() + "-" + std::to_string(dup) + ".log";
  }
  return candidate;
}

void AtomicLogger::rotate()
{
  if (_file)
  {
    const std::string archive = nextArchivePath();
    const std::string oldPath = _opts.directory + "/" + _opts.basename;

    // Close before renaming: an open handle makes the rename fail outright on
    // Windows.
    std::fclose(_file);
    _file = nullptr;
    std::rename(oldPath.c_str(), archive.c_str());
  }

  std::string path = _opts.directory + "/" + _opts.basename;
  std::error_code ec;
  fs::create_directories(_opts.directory, ec);
  _file = std::fopen(path.c_str(), "w");
  _bytesWritten = 0;
  _lastRotation = std::chrono::system_clock::now();

  if (_file == nullptr)
  {
    // The log directory went away or became unwritable under a running
    // process -- a volume unmounted, a disk filled, permissions changed. The
    // logger keeps going with no file and writeToOutput sends the lines to
    // stderr, so nothing is lost silently. Report the transition once rather
    // than on every rotation, and count them so it is visible to a
    // supervisor.
    if (_rotationFailures.fetch_add(1, std::memory_order_release) == 0)
    {
      const std::string reason = ec ? ec.message() : std::string("cannot open the file");
      std::fprintf(stderr, "flox: WARN log rotation failed for '%s': %s; log output goes to stderr\n",
                   path.c_str(), reason.c_str());
    }
  }
}

void AtomicLogger::writeToOutput(const LogEntry& entry)
{
  const char* levelStr =
      entry.level == LogLevel::Info ? "INFO" : entry.level == LogLevel::Warn ? "WARN"
                                                                             : "ERROR";

  char timebuf[32];
  formatTimestamp(entry.timestamp, timebuf, sizeof(timebuf));

  if (_file == nullptr)
  {
    // No file to write to -- the directory went away or became unwritable
    // under a running process. The line goes to stderr rather than nowhere:
    // a logger that cannot open its file is a reason to look at the console,
    // not a reason to discard what it was told. rotationFailures() stays the
    // machine-readable signal.
    std::fprintf(stderr, "flox: [%s] %s: %s\n", timebuf, levelStr, entry.message);
    return;
  }

  int written = std::fprintf(_file, "[%s] %s: %s\n", timebuf, levelStr, entry.message);
  if (written > 0)
  {
    _bytesWritten += written;
  }

  if (_opts.flushImmediately)
  {
    std::fflush(_file);
  }
}

void AtomicLogger::formatTimestamp(std::chrono::system_clock::time_point ts, char* buf, size_t bufSize)
{
  using namespace std::chrono;
  auto in_time_t = system_clock::to_time_t(ts);
  auto ms = duration_cast<milliseconds>(ts.time_since_epoch()) % 1000;

  std::tm tm;
#ifdef _WIN32
  localtime_s(&tm, &in_time_t);
#else
  localtime_r(&in_time_t, &tm);
#endif
  std::snprintf(buf, bufSize, "%04d%02d%02d-%02d%02d%02d.%03d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>(ms.count()));
}

}  // namespace flox
