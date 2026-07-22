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
#include <functional>
#include <map>
#include <vector>

namespace flox::replay
{

// Time-bucketed order-book snapshots. Maintains the full ladder per
// symbol from BookSnapshot/BookDelta events and, at each bucket
// boundary, emits the latest state observed inside the closed bucket
// as up to `levels` rows per side (one Row pairs the bid and ask at
// the same depth; the shorter side is zero-padded). Buckets with no
// book events for a symbol produce no rows; the state at the end of
// the walk is emitted as the trailing bucket.
//
// Book reconstruction is order-dependent across the WHOLE tape: a
// worker that starts mid-stream applies deltas to a ladder it never
// saw the snapshot for. The aggregator therefore refuses parallel
// runs — cloneEmpty()/merge() throw; use DataReader.run(..., n_threads=1).
class BookSnapshotBinAggregator final : public IAggregator
{
 public:
  struct Row
  {
    int64_t bucket_ts_ns{0};
    uint32_t symbol_id{0};
    uint16_t level{0};  // 0 = top of book
    uint16_t flags{0};  // bit 0: best bid >= best ask at this bucket
    int64_t bid_price_raw{0};
    int64_t bid_qty_raw{0};
    int64_t ask_price_raw{0};
    int64_t ask_qty_raw{0};
  };

  static constexpr uint16_t kFlagCrossed = 1u;

  // `bucket_ns` must be > 0, `levels` must be > 0. `event_filter` is
  // accepted for API parity; only book events ever contribute
  // (Trades-only filter yields an empty result).
  explicit BookSnapshotBinAggregator(
      int64_t bucket_ns,
      uint16_t levels = 20,
      AggregatorEventFilter event_filter = AggregatorEventFilter::BooksOnly,
      std::vector<uint32_t> symbol_filter = {});

  void onEvent(const ReplayEvent& ev) override;
  void finalize() override;
  std::unique_ptr<IAggregator> cloneEmpty() const override;
  void merge(const IAggregator& other) override;

  // Rows sorted by (bucket_ts_ns, symbol_id, level) ascending. Empty
  // before run() completes.
  const std::vector<Row>& result() const noexcept { return _rows; }

 private:
  struct SymState
  {
    std::map<int64_t, int64_t, std::greater<int64_t>> bids;  // price desc
    std::map<int64_t, int64_t> asks;                         // price asc
    int64_t cur_bucket{0};
    bool has_bucket{false};
  };

  bool symbolAllowed(uint32_t symbol_id) const noexcept;
  void emitCell(uint32_t symbol_id, const SymState& st);

  int64_t _bucket_ns;
  uint16_t _levels;
  AggregatorEventFilter _event_filter;
  std::vector<uint32_t> _symbol_filter;

  std::map<uint32_t, SymState> _states;
  std::vector<Row> _rows;
};

}  // namespace flox::replay
