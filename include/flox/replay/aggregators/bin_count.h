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
#include <map>
#include <tuple>
#include <vector>

namespace flox::replay
{

// Time-bucketed event counter. Each event is floored to its bucket
// (`exchange_ts_ns / bucket_ns * bucket_ns`) and the (bucket, optional
// dims) cell is incremented. Useful for "events per minute / per
// second / per bar" overviews without materialising the event stream.
//
// Optional split dimensions:
//   `by_side`   — split trades into BUY / SELL (book events keep
//                 side=0/aggregate; they have no native side concept).
//   `by_symbol` — split by symbol_id. Off by default; if you only care
//                 about one symbol, use `symbol_filter` instead and
//                 leave by_symbol=false to keep the result flat.
//
// Result rows carry both dims; the convention is `side=0` and
// `symbol_id=0` mean "aggregate across that dim". When `by_side=true`
// and `by_symbol=false`, output rows carry `symbol_id=0`; readers
// should not treat 0 as a real symbol id.
class BinCountAggregator final : public IAggregator
{
 public:
  struct Row
  {
    int64_t bucket_ts_ns{0};
    uint32_t symbol_id{0};  // 0 when by_symbol=false (aggregate)
    uint8_t side{0};        // 0 = aggregate, 1 = BUY, 2 = SELL
    uint64_t count{0};
  };

  // `bucket_ns` must be > 0. Other args mirror the rest of the
  // aggregator family: event_filter narrows which kinds count toward
  // the bucket (default Both); symbol_filter narrows which symbols
  // count (empty = all). Side dim only applies to trades; book events
  // always land in side=0 even when by_side=true.
  explicit BinCountAggregator(
      int64_t bucket_ns,
      bool by_side = false,
      bool by_symbol = false,
      AggregatorEventFilter event_filter = AggregatorEventFilter::Both,
      std::vector<uint32_t> symbol_filter = {});

  void onEvent(const ReplayEvent& ev) override;
  void finalize() override;

  // Rows sorted by (bucket_ts_ns, symbol_id, side) ascending. Empty
  // before run() completes.
  const std::vector<Row>& result() const noexcept { return _rows; }

 private:
  bool symbolAllowed(uint32_t symbol_id) const noexcept;

  int64_t _bucket_ns;
  bool _by_side;
  bool _by_symbol;
  AggregatorEventFilter _event_filter;
  std::vector<uint32_t> _symbol_filter;  // sorted ascending; empty = all

  // Storage keyed by (bucket, symbol, side). std::map gives sorted
  // iteration for finalize() without a separate sort step; insert is
  // O(log n) which is fine for typical capture sizes (≈ 10^4–10^6
  // distinct buckets even after the symbol×side split).
  std::map<std::tuple<int64_t, uint32_t, uint8_t>, uint64_t> _counts;
  std::vector<Row> _rows;
};

}  // namespace flox::replay
