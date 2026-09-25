/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <flox/common.h>

#include <cstddef>
#include <deque>
#include <unordered_map>

namespace flox
{

// Per-order cumulative fill watermark, shared by the connectors.
//
// Venues report a fill in two incompatible shapes, and some report both for
// the same execution: a cumulative total for the order (Bybit cumExecQty,
// Bitget accBaseVolume) and the size of the single execution (Bybit execQty,
// Bitget baseVolume). What the engine acts on is the delta -- OrderEvent's
// fillQty is what PositionTracker moves a position by -- and a delta published
// twice moves it twice.
//
// Expressing every report as a cumulative total and publishing only what it
// adds answers both at once: whichever channel reports an execution first
// advances the watermark and publishes the increment, and anything that
// repeats that execution (Bybit's other private topic, a snapshot re-pushed
// after a private resubscribe) adds nothing and publishes nothing.
//
// An entry deliberately outlives its order's terminal status, because the
// duplicate of the last fill arrives after it; entries are evicted in
// completion order once `historySize` orders have completed, which bounds the
// map without giving up that window.
//
// Not thread-safe by design: a connector touches it only from its own stream
// callback.
class FillWatermark
{
 public:
  explicit FillWatermark(size_t historySize = 4096) : _historySize(historySize) {}

  // Quantity already published for this order; zero if the order is unknown.
  Quantity reported(OrderId id) const
  {
    auto it = _entries.find(id);
    return (it != _entries.end()) ? it->second.cumulative : Quantity{};
  }

  // Advances the watermark to `cumulative` and returns what that adds. A zero
  // return means the report brought no new quantity and must not reach the bus
  // as a fill.
  Quantity advance(OrderId id, Quantity cumulative)
  {
    auto& entry = _entries[id];
    const int64_t increment = cumulative.raw() - entry.cumulative.raw();
    if (increment <= 0)
    {
      return Quantity{};
    }
    entry.cumulative = cumulative;
    return Quantity::fromRaw(increment);
  }

  // Marks the order terminal, making its entry eligible for eviction. Safe to
  // call more than once; only the first call queues the order.
  void complete(OrderId id)
  {
    auto& entry = _entries[id];
    if (entry.completed)
    {
      return;
    }
    entry.completed = true;
    _completed.push_back(id);
    while (_completed.size() > _historySize)
    {
      _entries.erase(_completed.front());
      _completed.pop_front();
    }
  }

 private:
  struct Entry
  {
    Quantity cumulative{};
    bool completed{false};
  };

  size_t _historySize;
  std::unordered_map<OrderId, Entry> _entries;
  std::deque<OrderId> _completed;
};

}  // namespace flox
