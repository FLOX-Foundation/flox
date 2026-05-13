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

// Time-bucketed quantity sum. Sums `trade.qty_raw` per
// (bucket, optional symbol, optional side) cell. The book-side
// equivalent (sum of bid/ask level sizes) is intentionally out of
// scope — books don't have a single "qty per event" the way trades
// do, and a useful book-volume metric is per-level not per-event.
//
// Result rows mirror BinCountAggregator's shape: one row per
// (bucket, symbol_id, side) with a `qty_raw` value. Bindings convert
// the raw int64 to whatever decimal-aware type the host expects (in
// Python: divide by `flox::Quantity::SCALE` for floats, or expose
// raw int64 alongside a precision hint).
class VolumeBinAggregator final : public IAggregator
{
 public:
  struct Row
  {
    int64_t bucket_ts_ns{0};
    uint32_t symbol_id{0};  // 0 when by_symbol=false (aggregate)
    uint8_t side{0};        // 0 = aggregate, 1 = BUY, 2 = SELL
    int64_t qty_raw{0};
  };

  // `bucket_ns` must be > 0. `event_filter` is accepted for API
  // parity with the rest of the family; only Trade events ever
  // contribute, so passing `BooksOnly` yields an empty result.
  explicit VolumeBinAggregator(
      int64_t bucket_ns,
      bool by_side = false,
      bool by_symbol = false,
      AggregatorEventFilter event_filter = AggregatorEventFilter::Trades,
      std::vector<uint32_t> symbol_filter = {});

  void onEvent(const ReplayEvent& ev) override;
  void finalize() override;
  std::unique_ptr<IAggregator> cloneEmpty() const override;
  void merge(const IAggregator& other) override;

  // Rows sorted by (bucket_ts_ns, symbol_id, side) ascending. Empty
  // before run() completes.
  const std::vector<Row>& result() const noexcept { return _rows; }

 private:
  bool symbolAllowed(uint32_t symbol_id) const noexcept;

  int64_t _bucket_ns;
  bool _by_side;
  bool _by_symbol;
  AggregatorEventFilter _event_filter;
  std::vector<uint32_t> _symbol_filter;

  std::map<std::tuple<int64_t, uint32_t, uint8_t>, int64_t> _qtys;
  std::vector<Row> _rows;
};

}  // namespace flox::replay
