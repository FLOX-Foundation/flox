/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/aggregation_policy.h"

namespace flox
{

class TickBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::Tick;

  explicit constexpr TickBarPolicy(uint32_t tickCount) noexcept
      : _tickCount(tickCount)
  {
  }

  [[nodiscard]] constexpr uint64_t param() const noexcept
  {
    return _tickCount;
  }

  [[nodiscard]] bool shouldClose(const TradeEvent& /*trade*/, const Bar& bar) const noexcept
  {
    // Close when we've accumulated enough trades
    return bar.tradeCount.raw() >= static_cast<int64_t>(_tickCount);
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept
  {
    updateOHLCV(trade, bar);
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept
  {
    initBarFromTrade(trade, bar);
  }

 private:
  uint32_t _tickCount;
};

static_assert(BarPolicy<TickBarPolicy>, "TickBarPolicy must satisfy BarPolicy concept");

}  // namespace flox
