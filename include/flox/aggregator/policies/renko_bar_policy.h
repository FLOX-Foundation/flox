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

#include <cmath>

namespace flox
{

class RenkoBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::Renko;

  // brickSize in raw price units (scaled by Price::Scale)
  explicit constexpr RenkoBarPolicy(int64_t brickSizeRaw) noexcept
      : _brickSizeRaw(brickSizeRaw)
  {
  }

  // Convenience: brick size from double
  static RenkoBarPolicy fromDouble(double brickSize) noexcept
  {
    return RenkoBarPolicy(Price::fromDouble(brickSize).raw());
  }

  [[nodiscard]] constexpr uint32_t param() const noexcept
  {
    // Return brick size in ticks (fits in 28 bits)
    return static_cast<uint32_t>(_brickSizeRaw);
  }

  [[nodiscard]] bool shouldClose(const TradeEvent& trade, const Bar& bar) const noexcept
  {
    // Renko closes when price moves brickSize away from bar's open
    const auto diff = std::abs(trade.trade.price.raw() - bar.open.raw());
    return diff >= _brickSizeRaw;
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept
  {
    // Renko bars only update close, high, low based on brick direction
    bar.close = trade.trade.price;
    bar.high = std::max(bar.high, trade.trade.price);
    bar.low = std::min(bar.low, trade.trade.price);

    const auto notional = Volume::fromRaw(
        (trade.trade.price.raw() * trade.trade.quantity.raw()) / Price::Scale);
    bar.volume += notional;
    bar.tradeCount += Quantity::fromRaw(1);
    if (trade.trade.isBuy)
    {
      bar.buyVolume += notional;
    }
    bar.endTime = fromUnixNs(trade.trade.exchangeTsNs);
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept
  {
    initBarFromTrade(trade, bar);
  }

 private:
  int64_t _brickSizeRaw;
};

static_assert(BarPolicy<RenkoBarPolicy>, "RenkoBarPolicy must satisfy BarPolicy concept");

}  // namespace flox
