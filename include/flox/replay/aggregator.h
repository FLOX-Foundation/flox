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
};

}  // namespace flox::replay
