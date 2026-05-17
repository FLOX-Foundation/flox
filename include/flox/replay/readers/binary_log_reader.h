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
#include <span>
#include <vector>

namespace flox::replay
{

class IAggregator;

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

  // Cross-block reorder window used by streamForEach / run() on
  // segments without the Sorted flag. Events with exchange_ts_ns
  // < (watermark - reorder_window_ns) cannot be emitted in sorted
  // order anymore (we already emitted past their position); the
  // reader throws FloxError with the observed delta when it sees
  // one. The default (10s) covers exchange-WS jitter and the
  // 99th-percentile of reconnect-induced cross-block inversions
  // measured on real md_collector tapes. Bump it for tapes with
  // longer real-world reconnect gaps.
  //
  // Memory bound: roughly reorder_window_ns × peak_event_rate ×
  // sizeof(ReplayEvent). At 10s × 10k ev/s burst that's ~36 MB —
  // 100× smaller than the legacy buffer-the-whole-segment path.
  int64_t reorder_window_ns{10'000'000'000};  // 10s default
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

  // Progress reporter invoked from inside `run()` at most once per
  // `interval`. `pct` is in [0.0, 1.0] based on bytes-of-segments
  // processed / total bytes (events-per-tape is not known up front).
  // `cursor_ts_ns` is the exchange timestamp of the most recent
  // dispatched event. Returning `false` requests cancellation —
  // `run()` stops the walk, calls `finalize()` once on every
  // aggregator with whatever partial state has been accumulated, and
  // returns `false` so the caller can distinguish a cancelled run
  // from a successful one. Returning `true` (or omitting the bool by
  // wrapping a `void`-returning lambda inside the std::function) lets
  // the run continue.
  using ProgressCallback = std::function<bool(double pct, int64_t cursor_ts_ns)>;
  void setProgressCallback(
      ProgressCallback cb,
      std::chrono::milliseconds interval = std::chrono::milliseconds(1000));
  void clearProgressCallback();

  // Sorted-order delivery. For segments without the Sorted flag the
  // entire segment is buffered into memory and stable_sort'ed before
  // dispatch — O(N_events × sizeof(ReplayEvent)) per segment. Use
  // this when the caller requires strict timestamp ordering across
  // events from the same segment (replay engine, tape_diff).
  bool forEach(EventCallback callback);
  bool forEachFrom(int64_t start_ts_ns, EventCallback callback);

  // Writer-order delivery — never buffers a whole segment. O(1)
  // memory per segment regardless of event count. Events arrive in
  // BinaryLogIterator order (the order the writer flushed them); for
  // Sorted segments this is identical to forEach. For unsorted
  // segments (e.g. tapes from external writers that don't set the
  // Sorted flag) the caller must tolerate writer-order arrival.
  //
  // Use this when memory matters more than strict timestamp order.
  // `run(panel)` below uses this path.
  bool streamForEach(EventCallback callback);
  bool streamForEachFrom(int64_t start_ts_ns, EventCallback callback);

  // Single-pass streaming aggregator dispatch. Walks the tape once
  // via `streamForEach`, forwarding each event to every aggregator's
  // onEvent, then calling finalize() on each. An empty span is a
  // no-op and performs no decompression.
  //
  // `n_threads` policy:
  //   0 (the default): auto — `min(segment_count,
  //                              std::thread::hardware_concurrency())`.
  //                    Caller doesn't have to know the tape layout
  //                    or core count; small captures stay
  //                    single-threaded, large captures saturate
  //                    available cores without oversubscription.
  //   1:               explicit single-thread (no clone/merge
  //                    overhead, useful for benchmarking).
  //   >1:              explicit worker count, capped at the segment
  //                    count internally — surplus workers are
  //                    not allocated.
  //
  // Multi-threaded path partitions the segment list across that
  // many worker threads. Each worker clones the panel via
  // `IAggregator::cloneEmpty()`, runs its segment slice, and the
  // reader then merges every worker's panel into the caller's
  // original instances via `IAggregator::merge()` before calling
  // finalize() once on each.
  //
  // Sliding-window aggregators (Peak / Quantile) cannot observe
  // windows that straddle worker boundaries. See
  // `IAggregator::merge` for the boundary semantics discussion.
  bool run(std::span<IAggregator* const> aggregators,
           std::size_t n_threads = 0);

  std::optional<std::pair<int64_t, int64_t>> timeRange() const;
  ReaderStats stats() const;
  std::vector<std::filesystem::path> segmentFiles() const;
  const std::vector<SegmentInfo>& segments() const;

 private:
  bool scanSegments();
  bool readSegment(const std::filesystem::path& path, EventCallback& callback);
  bool readSegmentFrom(const SegmentInfo& segment, int64_t start_ts_ns, EventCallback& callback);
  bool readSegmentStreaming(const std::filesystem::path& path, EventCallback& callback);
  bool readSegmentStreamingFrom(const SegmentInfo& segment, int64_t start_ts_ns,
                                EventCallback& callback);
  // Streams a single segment using the same intra-block sort + bounded
  // reorder buffer logic as readSegmentStreaming, but writes counter
  // updates into the caller-supplied ReaderStats. Used by parallel
  // run(panel, n_threads) so workers accumulate into thread-local
  // stats without locking shared _stats. The single-threaded
  // readSegmentStreaming delegates to this with `_stats`.
  bool streamSegmentWithStats(const std::filesystem::path& path,
                              EventCallback& callback, ReaderStats& stats);
  bool passesFilter(const ReplayEvent& event) const;

  ReaderConfig _config;
  ReaderStats _stats;
  std::vector<SegmentInfo> _segments;
  bool _scanned{false};
  ProgressCallback _progress_cb;
  std::chrono::milliseconds _progress_interval{1000};
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

  // Block-level positioning for intra-segment parallel workers. The
  // caller is responsible for picking valid block boundaries (use
  // `scanBlockOffsets` on the reader side to discover them); the
  // iterator does not verify the offset points at a real
  // CompressedBlockHeader. `setStopAtBlockEnd(true)` causes next()
  // to return false at the end of the current block instead of
  // auto-advancing to the next block — workers use this to confine
  // themselves to their assigned block range.
  bool seekToBlockOffset(uint64_t file_offset);
  void setStopAtBlockEnd(bool stop) { _stop_at_block_end = stop; }

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
  // Scratch buffer reused across sortBlockFramesInPlace calls. Empty
  // when the block was already monotonic (fast path) — only allocated
  // on the first unsorted block of the iterator's lifetime.
  std::vector<std::byte> _sort_scratch;
  size_t _block_offset{0};
  size_t _block_events_remaining{0};
  // Worker-mode flag: when true, `next()` returns false at the end of
  // the current block instead of calling loadNextBlock to advance.
  // Lets intra-segment parallel workers confine themselves to an
  // assigned block range.
  bool _stop_at_block_end{false};
};

}  // namespace flox::replay
