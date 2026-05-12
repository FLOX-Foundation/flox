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
#include "flox/replay/recording_metadata.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace flox::replay
{

struct MergedTradeRow
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t price_raw{0};
  int64_t qty_raw{0};
  uint64_t trade_id{0};
  uint32_t global_symbol_id{0};
  uint32_t tape_index{0};
  uint8_t side{0};
};

struct MergedBookRow
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t seq{0};
  uint32_t global_symbol_id{0};
  uint32_t tape_index{0};
  uint16_t bid_count{0};
  uint16_t ask_count{0};
  uint64_t level_offset{0};
  uint8_t event_type{0};  // 2 = snapshot, 3 = delta (matches reader convention)
};

struct MergedSymbol
{
  uint32_t global_id{0};
  std::string exchange;
  std::string name;
  int8_t price_precision{8};
  int8_t qty_precision{8};
};

struct MergedTapeReaderConfig
{
  std::vector<std::filesystem::path> tape_dirs;
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  // Filter applied post-rekey. Empty = all symbols.
  std::vector<uint32_t> symbol_filter;
};

class OverlappingBookStreamError : public std::runtime_error
{
 public:
  using std::runtime_error::runtime_error;
};

/// K-tape consumption primitive. Reads N `.floxlog` directories,
/// builds a global symbol registry by `(metadata.exchange, name)`
/// keying, and exposes the merged trade / book streams sorted by
/// exchange_ts_ns. Tie-break: (exchange_ts_ns, tape_index, source order).
///
/// V1 materialization: `read_trades` / `read_books` collect all events
/// across tapes, then sort. Memory bound = total events × row size.
/// A streaming heap-based iterator is a follow-up; the materialization
/// path keeps API parity with `DataReader`.
class MergedTapeReader
{
 public:
  explicit MergedTapeReader(MergedTapeReaderConfig config);

  MergedTapeReader(const MergedTapeReader&) = delete;
  MergedTapeReader& operator=(const MergedTapeReader&) = delete;

  // Global symbol registry, in global_id order.
  const std::vector<MergedSymbol>& symbols() const noexcept { return _symbols; }

  // Aggregate time range across all tapes (min first, max last).
  std::pair<int64_t, int64_t> timeRange() const noexcept { return _time_range; }

  // Merged sorted trade rows. Empty filter = all symbols.
  std::vector<MergedTradeRow> readTrades();

  // Merged sorted book headers + flat levels. levels are laid out as
  // bids then asks per event; each header's `level_offset` indexes into
  // the returned levels vector.
  std::pair<std::vector<MergedBookRow>, std::vector<BookLevel>> readBooks();

  // Per-tape contribution counts. Useful for "one tape is empty" debug.
  struct PerTapeStats
  {
    std::filesystem::path path;
    uint64_t trades{0};
    uint64_t books{0};
    int64_t first_event_ns{0};
    int64_t last_event_ns{0};
  };
  const std::vector<PerTapeStats>& perTapeStats() const noexcept
  {
    return _per_tape_stats;
  }

 private:
  void loadManifests();
  void detectBookOverlap();
  uint32_t globalIdForLocal(uint32_t tape_index, uint32_t local_id) const;
  bool symbolPassesFilter(uint32_t global_id) const;

  MergedTapeReaderConfig _config;

  std::vector<RecordingMetadata> _manifests;
  std::vector<MergedSymbol> _symbols;
  // Per tape: local_id (manifest entry) → global_id. -1 = unmapped.
  std::vector<std::vector<int64_t>> _local_to_global;
  std::vector<PerTapeStats> _per_tape_stats;
  std::pair<int64_t, int64_t> _time_range{0, 0};
};

}  // namespace flox::replay
