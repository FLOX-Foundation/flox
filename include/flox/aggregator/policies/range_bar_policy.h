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

class RangeBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::Range;

  // rangeSize in raw price units (scaled by Price::Scale)
  explicit constexpr RangeBarPolicy(int64_t rangeSizeRaw) noexcept
      : _rangeSizeRaw(rangeSizeRaw)
  {
  }

  // Convenience: range size from double
  static RangeBarPolicy fromDouble(double rangeSize) noexcept
  {
    return RangeBarPolicy(Price::fromDouble(rangeSize).raw());
  }

  [[nodiscard]] constexpr uint32_t param() const noexcept
  {
    // Return range size in ticks (fits in 28 bits)
    return static_cast<uint32_t>(_rangeSizeRaw);
  }

  [[nodiscard]] bool shouldClose(const TradeEvent& trade, const Bar& bar) const noexcept
  {
    // Range bar closes when high - low >= rangeSize
    const auto newHigh = std::max(bar.high, trade.trade.price);
    const auto newLow = std::min(bar.low, trade.trade.price);
    return (newHigh.raw() - newLow.raw()) >= _rangeSizeRaw;
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
  int64_t _rangeSizeRaw;
};

static_assert(BarPolicy<RangeBarPolicy>, "RangeBarPolicy must satisfy BarPolicy concept");

}  // namespace flox
