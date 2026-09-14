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
#include <vector>

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

  constexpr uint64_t param() const noexcept
  {
    // Same units as TimeframeId::renko(brickSize): the brick size in the
    // instrument's own price units (e.g. 100 for
    // RenkoBarPolicy::fromDouble(100.0)), not the internal
    // Price::Scale-scaled fixed-point _raw representation -- see
    // RangeBarPolicy::param() for the matching rationale and the
    // BarMatrix lookup this has to agree with.
    return static_cast<uint64_t>(_brickSizeRaw / Price::Scale);
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

    const auto notional = trade.trade.quantity * trade.trade.price;
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

  // A single trade can jump past several brick sizes at once (a real gap, or
  // just a thin book). `bar` here is the bar that just closed under the
  // ordinary single-brick rule above -- already emitted by the caller,
  // unmodified by this call. This fills in the bricks a continuous price
  // path would have produced between that bar and the trade that closed it:
  // one Bar per additional whole brick spanned, chained from `bar.open` in
  // the direction of the move, each exactly one brick tall. Empty when the
  // trade closed only the ordinary single brick (the common case), so
  // callers that ignore the return value see no change in behavior.
  [[nodiscard]] std::vector<Bar> gapBricks(const TradeEvent& trade, const Bar& bar) const
  {
    const int64_t openRaw = bar.open.raw();
    const int64_t diff = trade.trade.price.raw() - openRaw;
    const int64_t bricksSpanned = std::abs(diff) / _brickSizeRaw;

    std::vector<Bar> bricks;
    if (bricksSpanned < 2)
    {
      return bricks;
    }

    const int64_t sign = diff >= 0 ? 1 : -1;
    const auto tradeTs = fromUnixNs(trade.trade.exchangeTsNs);
    bricks.reserve(static_cast<size_t>(bricksSpanned - 1));
    for (int64_t i = 2; i <= bricksSpanned; ++i)
    {
      Bar brick{};
      brick.open = Price::fromRaw(openRaw + (i - 1) * sign * _brickSizeRaw);
      brick.close = Price::fromRaw(openRaw + i * sign * _brickSizeRaw);
      brick.high = Price::fromRaw(std::max(brick.open.raw(), brick.close.raw()));
      brick.low = Price::fromRaw(std::min(brick.open.raw(), brick.close.raw()));
      brick.startTime = tradeTs;
      brick.endTime = tradeTs;
      brick.reason = BarCloseReason::Threshold;
      bricks.push_back(brick);
    }
    return bricks;
  }

 private:
  int64_t _brickSizeRaw;
};

static_assert(BarPolicy<RenkoBarPolicy>, "RenkoBarPolicy must satisfy BarPolicy concept");

}  // namespace flox
