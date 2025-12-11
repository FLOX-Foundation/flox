/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/abstract_event_reader.h"

#include "flox/replay/readers/mmap_reader.h"
#include "flox/replay/readers/parallel_reader.h"

#include <atomic>
#include <mutex>

namespace flox::replay
{

class BinaryLogIteratorAdapter : public ISegmentReader
{
 public:
  explicit BinaryLogIteratorAdapter(const std::filesystem::path& path)
      : _iterator(path)
  {
    if (_iterator.isValid())
    {
      _iterator.loadIndex();
    }
  }

  bool isValid() const override { return _iterator.isValid(); }

  bool isCompressed() const override { return _iterator.isCompressed(); }

  bool hasIndex() const override { return _iterator.hasIndex(); }

  bool next(ReplayEvent& out) override { return _iterator.next(out); }

  void reset() override
  {
    // BinaryLogIterator doesn't have reset - recreate would be needed
    // For now this is a limitation
  }

  bool seekToTimestamp(int64_t target_ts_ns) override
  {
    return _iterator.seekToTimestamp(target_ts_ns);
  }

  const SegmentHeader& header() const override { return _iterator.header(); }

 private:
  BinaryLogIterator _iterator;
};

class MmapSegmentReaderAdapter : public ISegmentReader
{
 public:
  explicit MmapSegmentReaderAdapter(const std::filesystem::path& path)
      : _reader(path)
  {
    if (_reader.isValid() && _reader.hasIndex())
    {
      _reader.loadIndex();
    }
  }

  bool isValid() const override { return _reader.isValid(); }

  bool isCompressed() const override { return _reader.isCompressed(); }

  bool hasIndex() const override { return _reader.hasIndex(); }

  bool next(ReplayEvent& out) override { return _reader.next(out); }

  void reset() override { _reader.reset(); }

  bool seekToTimestamp(int64_t target_ts_ns) override
  {
    return _reader.seekToTimestamp(target_ts_ns);
  }

  const SegmentHeader& header() const override { return _reader.header(); }

 private:
  MmapSegmentReader _reader;
};

class BinaryLogReaderAdapter : public IMultiSegmentReader
{
 public:
  explicit BinaryLogReaderAdapter(const ReaderConfig& config) : _reader(config) {}

  uint64_t forEach(EventCallback callback) override
  {
    uint64_t count = 0;
    _reader.forEach([&count, &callback](const ReplayEvent& event)
                    {
      ++count;
      return callback(event); });
    return count;
  }

  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback) override
  {
    uint64_t count = 0;
    _reader.forEachFrom(start_ts_ns, [&count, &callback](const ReplayEvent& event)
                        {
      ++count;
      return callback(event); });
    return count;
  }

  uint64_t forEachWithProgress(
      EventCallback callback,
      ProgressCallback progress,
      uint64_t progress_interval_events) override
  {
    ReadProgress prog;
    prog.total_events = totalEvents();
    prog.total_segments = static_cast<uint32_t>(_reader.segments().size());

    uint64_t count = 0;
    uint64_t last_progress_at = 0;

    _reader.forEach([&](const ReplayEvent& event)
                    {
      ++count;
      prog.events_processed = count;
      prog.current_timestamp_ns = event.timestamp_ns;

      // Report progress periodically
      if (progress_interval_events > 0 && count - last_progress_at >= progress_interval_events)
      {
        progress(prog);
        last_progress_at = count;
      }

      return callback(event); });

    // Final progress report
    prog.segments_processed = prog.total_segments;
    progress(prog);

    return count;
  }

  const std::vector<SegmentInfo>& segments() const override
  {
    return _reader.segments();
  }

  uint64_t totalEvents() const override
  {
    uint64_t total = 0;
    for (const auto& seg : _reader.segments())
    {
      total += seg.event_count;
    }
    return total;
  }

 private:
  mutable BinaryLogReader _reader;
};

class MmapReaderAdapter : public IMultiSegmentReader
{
 public:
  explicit MmapReaderAdapter(const MmapReader::Config& config) : _reader(config) {}

  uint64_t forEach(EventCallback callback) override
  {
    return _reader.forEach(callback);
  }

  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback) override
  {
    return _reader.forEachFrom(start_ts_ns, callback);
  }

  uint64_t forEachWithProgress(
      EventCallback callback,
      ProgressCallback progress,
      uint64_t progress_interval_events) override
  {
    ReadProgress prog;
    prog.total_events = totalEvents();
    prog.total_segments = static_cast<uint32_t>(_reader.segments().size());

    uint64_t count = 0;
    uint64_t last_progress_at = 0;

    _reader.forEach([&](const ReplayEvent& event)
                    {
      ++count;
      prog.events_processed = count;
      prog.current_timestamp_ns = event.timestamp_ns;

      if (progress_interval_events > 0 && count - last_progress_at >= progress_interval_events)
      {
        progress(prog);
        last_progress_at = count;
      }

      return callback(event); });

    prog.segments_processed = prog.total_segments;
    progress(prog);

    return count;
  }

  const std::vector<SegmentInfo>& segments() const override
  {
    return _reader.segments();
  }

  uint64_t totalEvents() const override { return _reader.totalEvents(); }

 private:
  MmapReader _reader;
};

class ParallelReaderAdapter : public IMultiSegmentReader
{
 public:
  explicit ParallelReaderAdapter(const ParallelReaderConfig& config) : _reader(config) {}

  uint64_t forEach(EventCallback callback) override
  {
    return _reader.forEach(callback);
  }

  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback) override
  {
    // ParallelReader doesn't have forEachFrom, fall back to forEach with filter
    return _reader.forEach([start_ts_ns, &callback](const ReplayEvent& event)
                           {
      if (event.timestamp_ns >= start_ts_ns)
      {
        return callback(event);
      }
      return true; });
  }

  uint64_t forEachWithProgress(
      EventCallback callback,
      ProgressCallback progress,
      uint64_t progress_interval_events) override
  {
    ReadProgress prog;
    prog.total_events = totalEvents();
    prog.total_segments = static_cast<uint32_t>(_reader.segments().size());

    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> last_progress_at{0};
    std::mutex progress_mutex;

    _reader.forEach([&](const ReplayEvent& event)
                    {
      uint64_t current = count.fetch_add(1, std::memory_order_relaxed) + 1;
      uint64_t last = last_progress_at.load(std::memory_order_relaxed);

      if (progress_interval_events > 0 && current - last >= progress_interval_events)
      {
        if (last_progress_at.compare_exchange_strong(last, current))
        {
          std::lock_guard<std::mutex> lock(progress_mutex);
          prog.events_processed = current;
          prog.current_timestamp_ns = event.timestamp_ns;
          progress(prog);
        }
      }

      return callback(event); });

    prog.events_processed = count.load();
    prog.segments_processed = prog.total_segments;
    progress(prog);

    return count.load();
  }

  const std::vector<SegmentInfo>& segments() const override
  {
    return _reader.segments();
  }

  uint64_t totalEvents() const override
  {
    uint64_t total = 0;
    for (const auto& seg : _reader.segments())
    {
      total += seg.event_count;
    }
    return total;
  }

 private:
  ParallelReader _reader;
};

std::unique_ptr<ISegmentReader> createSegmentReader(
    const std::filesystem::path& segment_path,
    bool prefer_mmap)
{
  if (prefer_mmap)
  {
    auto reader = std::make_unique<MmapSegmentReaderAdapter>(segment_path);
    if (reader->isValid() && !reader->isCompressed())
    {
      return reader;
    }
  }

  // Fall back to iterator (supports compression)
  return std::make_unique<BinaryLogIteratorAdapter>(segment_path);
}

std::unique_ptr<IMultiSegmentReader> createMultiSegmentReader(
    const std::filesystem::path& data_dir,
    const ReaderFilter& filter,
    bool parallel,
    bool mmap)
{
  if (parallel)
  {
    ParallelReaderConfig config{
        .data_dir = data_dir,
        .from_ns = filter.from_ns,
        .to_ns = filter.to_ns,
        .symbols = filter.symbols,
    };
    return std::make_unique<ParallelReaderAdapter>(config);
  }

  if (mmap)
  {
    MmapReader::Config config{
        .data_dir = data_dir,
        .from_ns = filter.from_ns,
        .to_ns = filter.to_ns,
        .symbols = filter.symbols,
    };
    return std::make_unique<MmapReaderAdapter>(config);
  }

  // Default: sequential reader
  ReaderConfig config{
      .data_dir = data_dir,
      .from_ns = filter.from_ns,
      .to_ns = filter.to_ns,
      .symbols = filter.symbols,
  };
  return std::make_unique<BinaryLogReaderAdapter>(config);
}

}  // namespace flox::replay
