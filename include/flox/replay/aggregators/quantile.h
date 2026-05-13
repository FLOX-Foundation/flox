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
#include <deque>
#include <unordered_map>
#include <vector>

namespace flox::replay
{

// Window-count distribution + quantile lookup. For each scale
// `window_ns`, observes the count of events inside a sliding window
// of that width every time an event arrives, accumulating those
// observations into a histogram. At `finalize()` resolves each
// requested quantile to the count threshold below which that fraction
// of observed windows lie.
//
// Example: with `window_ns_list = {1_000_000}` (1 ms) and
// `quantiles = {0.5, 0.95, 0.99}`, the result reports the median, the
// 95th-percentile, and the 99th-percentile of "trades per 1 ms window"
// across the whole tape — a baseline for how busy the captured period
// was at the millisecond scale.
//
// Histogram storage keeps a per-scale `unordered_map<count_value,
// occurrences>` rather than a vector of all observations, because the
// distinct count-value range is small (bounded by the peak burst size,
// typically in the dozens) while the observation count is huge (one
// per event per scale).
class QuantileAggregator final : public IAggregator
{
 public:
  struct Row
  {
    int64_t window_ns{0};
    double quantile{0.0};
    uint64_t count{0};  // smallest C such that fraction(observed ≤ C) ≥ quantile
  };

  // `window_ns_list` and `quantiles` must be non-empty. Every window
  // must be > 0, every quantile in (0.0, 1.0]. Duplicates are
  // collapsed; quantiles end up sorted ascending in the result.
  explicit QuantileAggregator(
      std::vector<int64_t> window_ns_list,
      std::vector<double> quantiles,
      AggregatorEventFilter event_filter = AggregatorEventFilter::Trades,
      std::vector<uint32_t> symbol_filter = {});

  void onEvent(const ReplayEvent& ev) override;
  void finalize() override;
  std::unique_ptr<IAggregator> cloneEmpty() const override;
  void merge(const IAggregator& other) override;

  // Result rows ordered by (window_ns input order, quantile asc).
  const std::vector<Row>& result() const noexcept { return _rows; }

 private:
  bool symbolAllowed(uint32_t symbol_id) const noexcept;

  struct Scale
  {
    int64_t window_ns;
    std::deque<int64_t> sliding;
    std::unordered_map<uint64_t, uint64_t> histogram;  // count_value → occurrences
    uint64_t total_observations{0};
  };

  std::vector<Scale> _scales;
  std::vector<double> _quantiles;  // sorted ascending, deduplicated
  AggregatorEventFilter _event_filter;
  std::vector<uint32_t> _symbol_filter;
  std::vector<Row> _rows;
};

}  // namespace flox::replay
