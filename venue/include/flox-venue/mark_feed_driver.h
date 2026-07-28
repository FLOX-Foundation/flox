/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/index_feed.h"
#include "flox-venue/messages.h"

#include <cstdint>

namespace flox::venue
{

template <class Risk>
class MarkFeedDriver
{
 public:
  MarkFeedDriver(IndexAggregator& idx, MarkPrice& mark, Risk& risk, SymbolId sym, int32_t clampBps)
      : idx_(idx), mark_(mark), risk_(risk), sym_(sym), clampBps_(clampBps)
  {
    (void)clampBps_;  // MarkPrice already holds its clamp; kept for symmetry/config
  }

  // Update the mark from the current feed. `lastTradeRaw`/`impactMidRaw` are the
  // perp's own signals (0 = unavailable). Returns true if a fresh mark was
  // published; false if the feed was stale and liquidations were paused.
  bool onTick(int64_t nowNs, int64_t lastTradeRaw, int64_t impactMidRaw)
  {
    if (!idx_.hasIndex(nowNs))
    {
      risk_.setLiquidationsPaused(true);  // feed outage -> freeze liquidations
      paused_ = true;
      return false;
    }
    if (paused_)
    {
      risk_.setLiquidationsPaused(false);  // feed recovered
      paused_ = false;
    }
    mark_.setIndex(idx_.index(nowNs));
    if (impactMidRaw > 0)
    {
      mark_.setMid(Price::fromRaw(impactMidRaw));
    }
    if (lastTradeRaw > 0)
    {
      mark_.setLast(Price::fromRaw(lastTradeRaw));
    }
    risk_.setMark(sym_, mark_.value());
    lastPublishNs_ = nowNs;
    return true;
  }

  bool paused() const noexcept { return paused_; }

  // Age of the last published mark (ns); -1 before the first publish. Feed this
  // into Gauges::markPriceAgeNs for the feed-lag alert.
  int64_t markAgeNs(int64_t nowNs) const noexcept
  {
    return lastPublishNs_ < 0 ? -1 : nowNs - lastPublishNs_;
  }

 private:
  IndexAggregator& idx_;
  MarkPrice& mark_;
  Risk& risk_;
  SymbolId sym_;
  int32_t clampBps_;
  bool paused_{false};
  int64_t lastPublishNs_{-1};
};

}  // namespace flox::venue
