/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/replay/readers/binary_log_reader.h"

#include <cstdint>
#include <memory>

namespace flox::replay
{

// Event-type filter applied inside each aggregator's onEvent so a single
// decompression pass can feed aggregators that disagree on which events
// they care about. The reader's `run(...)` dispatches every event to every
// aggregator; the aggregator itself decides whether to ignore it.
enum class AggregatorEventFilter : uint8_t
{
  Trades = 1,     // TradeRecord events only
  BooksOnly = 2,  // BookSnapshot + BookDelta events only
  Both = 3,       // both kinds
};

// Streaming aggregator contract. The reader walks the tape once,
// dispatches each ReplayEvent to every attached aggregator's onEvent,
// and calls finalize() once at the end of the walk.
//
// Implementations MUST be self-contained — no I/O, no allocations on
// the hot path, no Python crossings. Filters (event-type, symbol)
// belong inside onEvent so a single pass serves heterogeneous filters
// without re-walking the tape.
class IAggregator
{
 public:
  virtual ~IAggregator() = default;

  // Called once per ReplayEvent in (exchange_ts_ns, segment-order) order.
  // The event reference is only valid for the duration of the call.
  virtual void onEvent(const ReplayEvent& ev) = 0;

  // Called once at the end of the walk, after the last onEvent. Use for
  // sort + dedup + top-N finalisation steps that are cheaper at the end
  // than maintained incrementally. Default no-op for aggregators whose
  // running state is already the result.
  virtual void finalize() {}

  // Construct a fresh aggregator of the same concrete type with the
  // same configuration (window list, filters, bucket size, etc.) but
  // empty accumulation state. Used by parallel `run(panel, n_threads)`
  // to seed per-worker panel copies without the caller having to know
  // the concrete type. Returned ownership is the caller's.
  virtual std::unique_ptr<IAggregator> cloneEmpty() const = 0;

  // Merge another aggregator's pre-finalize state into this one. The
  // other instance must be of the same concrete type — implementations
  // typically dynamic_cast and throw std::invalid_argument on mismatch.
  // The reader calls merge() between workers and the user's panel
  // before the final finalize() so callers always read finalized
  // results from the original panel instances.
  //
  // Boundary semantics: parallel `run()` partitions segments across
  // workers; sliding-window aggregators (Peak / Quantile) cannot
  // observe windows that straddle worker boundaries. The merge of
  // such aggregators combines what each worker captured within its
  // share, so peaks / quantile observations on the boundary regions
  // (≤ max(window_ns) per partition seam) may be under-counted. See
  // the T020 tracker entry for the full discussion.
  virtual void merge(const IAggregator& other) = 0;
};

}  // namespace flox::replay
