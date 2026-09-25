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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

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

  void update(const TradeEvent& trade, Bar& bar) const noexcept
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

  // The complete close step for the trade that crossed a brick boundary.
  // `bar` enters as the brick that was forming and leaves as the brick that
  // opens next; every bar to publish is handed to `emit` in order.
  //
  // A brick is a price construct, so both ends of the close are fixed by the
  // grid and not by the trade: the brick closes *at* the boundary the trade
  // reached, and the next brick opens at that same boundary. Leaving those
  // two steps to the caller -- publish the bar as it stood, then initBar()
  // at the trade price -- produced bricks that were not one brick tall, that
  // pointed against the move, and that walked the grid off by the crossing
  // trade's overshoot on every close. Computing both from one set of
  // boundaries here is what keeps them the same number.
  template <typename Emit>
  void closeAndReopen(const TradeEvent& trade, Bar& bar, Emit&& emit) const
  {
    const int64_t openRaw = bar.open.raw();
    const int64_t diff = trade.trade.price.raw() - openRaw;
    const int64_t sign = diff >= 0 ? 1 : -1;
    // shouldClose() is the precondition, so this is at least 1.
    const int64_t spanned = (sign * diff) / _brickSizeRaw;
    const auto tradeTs = fromUnixNs(trade.trade.exchangeTsNs);

    const auto boundary = [&](int64_t brickIndex)
    { return Price::fromRaw(openRaw + brickIndex * sign * _brickSizeRaw); };

    // The crossing trade completes this brick, so its notional and its tick
    // are counted here -- not in the brick that opens next, which no trade
    // has touched yet.
    update(trade, bar);
    bar.close = boundary(1);
    // The crossing trade is the only one that can sit past the boundary: an
    // earlier one that did would have closed this brick already. How far it
    // overshot is carried by the bricks that follow, so the extreme on the
    // side of the move is the boundary itself rather than that trade's
    // price -- otherwise a 100 -> 110 brick would report a high of 155 that
    // the next four bricks report again. The opposite side keeps whatever
    // retracement the trades actually made.
    if (sign > 0)
    {
      bar.high = bar.close;
    }
    else
    {
      bar.low = bar.close;
    }
    bar.reason = BarCloseReason::Threshold;
    emit(std::as_const(bar));

    // One synthetic brick per further whole brick width the trade spanned --
    // the bricks a continuous price path would have produced. They carry no
    // volume and no trade count: no trade happened at those prices.
    for (int64_t i = 2; i <= spanned; ++i)
    {
      emit(syntheticBrick(boundary(i - 1), boundary(i), tradeTs, BarCloseReason::Threshold));
    }

    openAtBoundary(boundary(spanned), trade, bar);
  }

 private:
  static Bar syntheticBrick(Price open, Price close, TimePoint ts, BarCloseReason reason) noexcept
  {
    Bar brick{};
    brick.open = open;
    brick.close = close;
    brick.high = std::max(open, close);
    brick.low = std::min(open, close);
    brick.startTime = ts;
    brick.endTime = ts;
    brick.reason = reason;
    return brick;
  }

  // The brick that opens after a close starts on the grid and is empty: the
  // trade that opened it was already counted in the brick it completed.
  // Its close tracks that trade's price so a forced flush still reports
  // where the price stands, while shouldClose() keeps measuring from the
  // boundary in `open`.
  static void openAtBoundary(Price boundary, const TradeEvent& trade, Bar& bar) noexcept
  {
    const auto price = trade.trade.price;
    const auto ts = fromUnixNs(trade.trade.exchangeTsNs);

    bar.open = boundary;
    bar.close = price;
    bar.high = std::max(boundary, price);
    bar.low = std::min(boundary, price);
    bar.volume = Volume{};
    bar.buyVolume = Volume{};
    bar.tradeCount = Quantity{};
    bar.startTime = ts;
    bar.endTime = ts;
    bar.reason = BarCloseReason::Threshold;
  }

  int64_t _brickSizeRaw;
};

static_assert(BarPolicy<RenkoBarPolicy>, "RenkoBarPolicy must satisfy BarPolicy concept");

}  // namespace flox
