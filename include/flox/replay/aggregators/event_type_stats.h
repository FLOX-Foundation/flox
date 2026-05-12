/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/replay/aggregator.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace flox::replay
{

// Per-symbol counters split by event kind. Cheapest of the native
// aggregators; useful as the "what's in this tape" overview and as a
// reference impl for the IAggregator contract.
//
// Result shape: `result()` returns a flat vector of (symbol_id, trades,
// book_snapshots, book_deltas) rows in symbol-id order. Bindings convert
// this to whatever the host language considers idiomatic (a Python dict
// keyed by symbol_id, etc) without further computation.
class EventTypeStatsAggregator final : public IAggregator
{
 public:
  struct PerSymbolRow
  {
    uint32_t symbol_id{0};
    uint64_t trades{0};
    uint64_t book_snapshots{0};
    uint64_t book_deltas{0};
  };

  // `symbol_filter` empty = all symbols. `event_filter` controls which
  // event kinds are counted; rows for filtered-out kinds stay zero.
  explicit EventTypeStatsAggregator(
      AggregatorEventFilter event_filter = AggregatorEventFilter::Both,
      std::vector<uint32_t> symbol_filter = {});

  void onEvent(const ReplayEvent& ev) override;
  void finalize() override;

  // Sorted-by-symbol-id flat result. Available after run() completes
  // (the reader calls finalize() on its way out). Calling result()
  // before run() returns an empty vector.
  const std::vector<PerSymbolRow>& result() const noexcept { return _rows; }

 private:
  bool symbolAllowed(uint32_t symbol_id) const noexcept;

  AggregatorEventFilter _event_filter;
  std::vector<uint32_t> _symbol_filter;  // sorted ascending; empty = all
  std::unordered_map<uint32_t, PerSymbolRow> _counts;
  std::vector<PerSymbolRow> _rows;  // populated in finalize()
};

}  // namespace flox::replay
