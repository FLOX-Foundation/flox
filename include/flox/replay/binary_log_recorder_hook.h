/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/replay/recording_metadata.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace flox::replay
{

struct RecorderStats
{
  uint64_t trades_written{0};
  uint64_t book_updates_written{0};
  uint64_t bytes_written{0};
  uint64_t segments_created{0};
  uint64_t errors{0};
};

struct BinaryLogRecorderHookConfig
{
  std::filesystem::path output_dir;
  uint64_t max_segment_bytes{256ull << 20};
  uint8_t exchange_id{0};
  CompressionType compression{CompressionType::None};
  std::optional<RecordingMetadata> metadata;
};

/// Recorder hook backed by BinaryLogWriter. Owns the writer; routes
/// trade and book-update events from a runner or live engine into the
/// binary log without crossing a managed-language boundary.
///
/// Attach via the C-API helper
/// `flox_binary_log_recorder_hook_as_recorder()`, which returns a
/// `FloxMarketDataRecorderHandle` usable with
/// `flox_runner_set_market_data_recorder` or
/// `flox_live_engine_set_market_data_recorder`.
class BinaryLogRecorderHook
{
 public:
  explicit BinaryLogRecorderHook(BinaryLogRecorderHookConfig config);
  ~BinaryLogRecorderHook();

  BinaryLogRecorderHook(const BinaryLogRecorderHook&) = delete;
  BinaryLogRecorderHook& operator=(const BinaryLogRecorderHook&) = delete;
  BinaryLogRecorderHook(BinaryLogRecorderHook&&) noexcept = delete;
  BinaryLogRecorderHook& operator=(BinaryLogRecorderHook&&) noexcept = delete;

  // Lifecycle. Idempotent: start() on an already-started hook is a
  // no-op, same for stop().
  void start();
  void stop();
  bool isRecording() const noexcept
  {
    return _recording.load(std::memory_order_relaxed);
  }

  // Event ingest. Raw int64 prices/qty match the on-disk format, so
  // there's no double conversion in the hot path.
  void onTrade(uint32_t symbol_id, int64_t price_raw, int64_t qty_raw,
               bool is_buy, int64_t exchange_ts_ns, int64_t recv_ts_ns) noexcept;
  void onBookUpdate(uint32_t symbol_id, bool is_snapshot,
                    const BookLevel* bids, uint32_t n_bids,
                    const BookLevel* asks, uint32_t n_asks,
                    int64_t exchange_ts_ns, int64_t recv_ts_ns) noexcept;

  // Symbol metadata + maintenance.
  void addSymbol(const SymbolInfo& info);
  void flush();
  RecorderStats stats() const noexcept;
  std::filesystem::path currentSegmentPath() const;

 private:
  BinaryLogRecorderHookConfig _config;
  std::unique_ptr<BinaryLogWriter> _writer;
  std::atomic<bool> _recording{false};
  mutable RecorderStats _stats;
};

}  // namespace flox::replay
