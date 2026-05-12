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

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace flox::replay
{

// Top-N busiest fixed-width windows per scale. Finds the time
// intervals that pack the most events into a fixed duration —
// "where are the bursts" — across the captured tape.
//
// For each scale `window_ns` in the constructor list, maintains:
//   - a sliding deque of in-window event timestamps, pruned to
//     the (open, closed] interval ending at the latest event;
//   - a bounded min-heap of candidate `(count, start_ns)` peaks.
// At `finalize()` the heap is drained, sorted by count descending,
// and deduplicated by suppressing peaks within 3×window_ns of a
// stronger one already kept (matches the Python prototype at
// `new_article/scan_full.py`).
//
// `result()` is indexed by scale order: rows for scale 0 come first,
// then scale 1, etc. Within a scale the rows are sorted by count
// descending (the peak survivors of the dedup pass).
class PeakAggregator final : public IAggregator
{
 public:
  struct Row
  {
    int64_t window_ns{0};
    uint64_t count{0};
    int64_t start_ns{0};
  };

  // `window_ns_list` must be non-empty; every entry must be > 0.
  // `top_n` is the post-dedup cap per scale. `oversample_factor`
  // (≥1) is the multiplier on the in-flight candidate heap size:
  // we keep `top_n * oversample_factor` candidates per scale during
  // the walk and dedup at the end. Default 100 leaves room for
  // bursts that cluster within 3×window of an earlier kept peak
  // (which would otherwise get suppressed before a distinct burst
  // arrives).
  explicit PeakAggregator(
      std::vector<int64_t> window_ns_list,
      std::size_t top_n = 10,
      std::size_t oversample_factor = 100,
      AggregatorEventFilter event_filter = AggregatorEventFilter::Trades,
      std::vector<uint32_t> symbol_filter = {});

  void onEvent(const ReplayEvent& ev) override;
  void finalize() override;

  const std::vector<Row>& result() const noexcept { return _rows; }

 private:
  bool symbolAllowed(uint32_t symbol_id) const noexcept;

  struct Scale
  {
    int64_t window_ns;
    std::deque<int64_t> sliding;                     // timestamps in (t-w, t]
    std::vector<std::pair<uint64_t, int64_t>> heap;  // min-heap on .first
  };

  std::vector<Scale> _scales;
  std::size_t _top_n;
  std::size_t _candidate_budget;  // top_n * oversample_factor per scale
  AggregatorEventFilter _event_filter;
  std::vector<uint32_t> _symbol_filter;
  std::vector<Row> _rows;
};

}  // namespace flox::replay
