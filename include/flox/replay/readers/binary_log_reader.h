/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/replay/binary_format_v1.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <vector>

namespace flox::replay
{

struct TimeRange
{
  int64_t start_ns{0};
  int64_t end_ns{0};

  bool empty() const { return start_ns == 0 && end_ns == 0; }

  std::chrono::nanoseconds duration() const
  {
    return std::chrono::nanoseconds(end_ns - start_ns);
  }

  double durationSeconds() const
  {
    return static_cast<double>(end_ns - start_ns) / 1e9;
  }

  bool contains(int64_t timestamp_ns) const
  {
    return timestamp_ns >= start_ns && timestamp_ns <= end_ns;
  }
};

namespace time_utils
{

inline int64_t toNanos(std::chrono::system_clock::time_point tp)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

inline std::chrono::system_clock::time_point fromNanos(int64_t ns)
{
  using namespace std::chrono;
  return time_point_cast<system_clock::duration>(
      time_point<system_clock, nanoseconds>(nanoseconds(ns)));
}

inline int64_t nowNanos()
{
  return toNanos(std::chrono::system_clock::now());
}

inline int64_t secondsToNanos(int64_t seconds)
{
  return seconds * 1'000'000'000LL;
}

inline int64_t millisToNanos(int64_t millis)
{
  return millis * 1'000'000LL;
}

inline int64_t microsToNanos(int64_t micros)
{
  return micros * 1'000LL;
}

inline double nanosToSeconds(int64_t ns)
{
  return static_cast<double>(ns) / 1e9;
}

}  // namespace time_utils

struct ReaderConfig
{
  std::filesystem::path data_dir;
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  std::set<uint32_t> symbols;
  bool verify_crc{true};
};

struct DatasetSummary
{
  std::filesystem::path data_dir;

  int64_t first_event_ns{0};
  int64_t last_event_ns{0};

  uint64_t total_events{0};
  uint32_t segment_count{0};
  uint64_t total_bytes{0};

  std::set<uint32_t> symbols;

  uint32_t segments_with_index{0};
  uint32_t segments_without_index{0};

  bool empty() const { return total_events == 0; }

  std::chrono::nanoseconds duration() const
  {
    return std::chrono::nanoseconds(last_event_ns - first_event_ns);
  }

  double durationSeconds() const
  {
    return static_cast<double>(last_event_ns - first_event_ns) / 1e9;
  }

  double durationMinutes() const { return durationSeconds() / 60.0; }
  double durationHours() const { return durationMinutes() / 60.0; }

  bool fullyIndexed() const { return segments_without_index == 0 && segment_count > 0; }
};

struct ReaderStats
{
  uint64_t files_read{0};
  uint64_t events_read{0};
  uint64_t trades_read{0};
  uint64_t book_updates_read{0};
  uint64_t bytes_read{0};
  uint64_t crc_errors{0};
};

struct ReplayEvent
{
  EventType type{};
  int64_t timestamp_ns{0};

  TradeRecord trade{};

  BookRecordHeader book_header{};
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
};

struct SegmentInfo
{
  std::filesystem::path path;
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t event_count{0};
  bool has_index{false};
  uint64_t index_offset{0};
};

class BinaryLogReader
{
 public:
  explicit BinaryLogReader(ReaderConfig config);
  ~BinaryLogReader();

  BinaryLogReader(const BinaryLogReader&) = delete;
  BinaryLogReader& operator=(const BinaryLogReader&) = delete;
  BinaryLogReader(BinaryLogReader&&) noexcept;
  BinaryLogReader& operator=(BinaryLogReader&&) noexcept;

  static DatasetSummary inspect(const std::filesystem::path& data_dir);
  static DatasetSummary inspectWithSymbols(const std::filesystem::path& data_dir);

  DatasetSummary summary();
  uint64_t count();
  std::set<uint32_t> availableSymbols();

  using EventCallback = std::function<bool(const ReplayEvent&)>;
  bool forEach(EventCallback callback);
  bool forEachFrom(int64_t start_ts_ns, EventCallback callback);

  std::optional<std::pair<int64_t, int64_t>> timeRange() const;
  ReaderStats stats() const;
  std::vector<std::filesystem::path> segmentFiles() const;
  const std::vector<SegmentInfo>& segments() const;

 private:
  bool scanSegments();
  bool readSegment(const std::filesystem::path& path, EventCallback& callback);
  bool readSegmentFrom(const SegmentInfo& segment, int64_t start_ts_ns, EventCallback& callback);
  bool passesFilter(const ReplayEvent& event) const;

  ReaderConfig _config;
  ReaderStats _stats;
  std::vector<SegmentInfo> _segments;
  bool _scanned{false};
};

class BinaryLogIterator
{
 public:
  explicit BinaryLogIterator(const std::filesystem::path& segment_path);
  ~BinaryLogIterator();

  BinaryLogIterator(const BinaryLogIterator&) = delete;
  BinaryLogIterator& operator=(const BinaryLogIterator&) = delete;

  bool next(ReplayEvent& out);

  const SegmentHeader& header() const { return _header; }
  bool isValid() const { return _file != nullptr; }
  bool isCompressed() const { return _header.isCompressed(); }

  bool seekToTimestamp(int64_t target_ts_ns);
  bool loadIndex();
  bool hasIndex() const { return !_index_entries.empty(); }

 private:
  bool nextUncompressed(ReplayEvent& out);
  bool nextCompressed(ReplayEvent& out);
  bool loadNextBlock();
  bool parseFrame(EventType type, const std::byte* data, size_t size, ReplayEvent& out);

  std::FILE* _file{nullptr};
  SegmentHeader _header{};
  std::vector<std::byte> _payload_buffer;
  std::vector<IndexEntry> _index_entries;

  std::vector<std::byte> _block_data;
  size_t _block_offset{0};
  size_t _block_events_remaining{0};
};

}  // namespace flox::replay
