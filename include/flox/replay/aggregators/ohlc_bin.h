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

// Time-bucketed price OHLC. For each (bucket, optional symbol) cell,
// records first/last/min/max of `trade.price_raw`. Empty buckets
// produce no row; the caller is responsible for forward-filling if a
// dense series is needed.
//
// Trade-only: book events have no single "price per event" the way
// trades do. Passing event_filter=BooksOnly yields an empty result.
//
// by_side is intentionally out of scope: an "open price for buys" is
// not a generally useful primitive. Callers who want per-side flow
// metrics should pair this with VolumeBinAggregator(by_side=true).
class OHLCBinAggregator final : public IAggregator
{
 public:
  struct Row
  {
    int64_t bucket_ts_ns{0};
    uint32_t symbol_id{0};  // 0 when by_symbol=false (aggregate)
    int64_t open_raw{0};
    int64_t high_raw{0};
    int64_t low_raw{0};
    int64_t close_raw{0};
  };

  // `bucket_ns` must be > 0. `event_filter` is accepted for API
  // parity; only trade events ever contribute.
  explicit OHLCBinAggregator(
      int64_t bucket_ns,
      bool by_symbol = false,
      AggregatorEventFilter event_filter = AggregatorEventFilter::Trades,
      std::vector<uint32_t> symbol_filter = {});

  void onEvent(const ReplayEvent& ev) override;
  void finalize() override;
  std::unique_ptr<IAggregator> cloneEmpty() const override;
  void merge(const IAggregator& other) override;

  // Rows sorted by (bucket_ts_ns, symbol_id) ascending. Empty before
  // run() completes.
  const std::vector<Row>& result() const noexcept { return _rows; }

 private:
  struct Cell
  {
    int64_t open_ts_ns{0};
    int64_t close_ts_ns{0};
    int64_t open_raw{0};
    int64_t close_raw{0};
    int64_t high_raw{0};
    int64_t low_raw{0};
  };

  bool symbolAllowed(uint32_t symbol_id) const noexcept;

  int64_t _bucket_ns;
  bool _by_symbol;
  AggregatorEventFilter _event_filter;
  std::vector<uint32_t> _symbol_filter;

  std::map<std::tuple<int64_t, uint32_t>, Cell> _cells;
  std::vector<Row> _rows;
};

}  // namespace flox::replay
