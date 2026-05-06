/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"
#include "flox/replay/abstract_event_reader.h"

#include <cstdint>
#include <vector>

namespace flox
{

/// Replays OHLCV close prices as a stream of synthetic trade events.
/// Used by BacktestRunner / WalkForwardRunner / GridSearch when the
/// user supplies bar data and the strategy reacts to `on_trade`.
class OhlcvReplaySource : public replay::IMultiSegmentReader
{
 public:
  struct Bar
  {
    int64_t ts_ns;
    int64_t price_raw;
    SymbolId symbol_id;
  };

  explicit OhlcvReplaySource(std::vector<Bar> bars);

  uint64_t forEach(EventCallback cb) override;
  uint64_t forEachFrom(int64_t start_ns, EventCallback cb) override;
  const std::vector<replay::SegmentInfo>& segments() const override
  {
    return _segs;
  }
  uint64_t totalEvents() const override { return _bars.size(); }

 private:
  static replay::ReplayEvent make(const Bar& b);

  std::vector<Bar> _bars;
  std::vector<replay::SegmentInfo> _segs;
};

}  // namespace flox
