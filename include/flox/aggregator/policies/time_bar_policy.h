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

#include <chrono>

namespace flox
{

class TimeBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::Time;

  explicit constexpr TimeBarPolicy(std::chrono::seconds interval) noexcept
      : _interval(interval)
  {
  }

  [[nodiscard]] constexpr uint32_t param() const noexcept
  {
    return static_cast<uint32_t>(_interval.count());
  }

  [[nodiscard]] bool shouldClose(const TradeEvent& trade, const Bar& bar) const noexcept
  {
    const auto tradeTs = fromUnixNs(trade.trade.exchangeTsNs);
    const auto alignedTradeTs = alignToInterval(tradeTs);
    return alignedTradeTs != bar.startTime;
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept
  {
    // For time bars, endTime is fixed at startTime + interval, don't update it
    bar.high = std::max(bar.high, trade.trade.price);
    bar.low = std::min(bar.low, trade.trade.price);
    bar.close = trade.trade.price;

    const auto notional = Volume::fromRaw(
        (trade.trade.price.raw() * trade.trade.quantity.raw()) / Price::Scale);
    bar.volume += notional;
    bar.tradeCount += Quantity::fromRaw(1);

    if (trade.trade.isBuy)
    {
      bar.buyVolume += notional;
    }
    // Note: we intentionally don't update endTime here; it's set by initBar
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept
  {
    initBarFromTrade(trade, bar);
    bar.startTime = alignToInterval(bar.startTime);
    bar.endTime = bar.startTime + _interval;
  }

 private:
  [[nodiscard]] TimePoint alignToInterval(TimePoint tp) const noexcept
  {
    const auto epoch = tp.time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(epoch);
    const auto snapped = (secs.count() / _interval.count()) * _interval.count();
    return TimePoint(std::chrono::seconds(snapped));
  }

  std::chrono::seconds _interval;
};

static_assert(BarPolicy<TimeBarPolicy>, "TimeBarPolicy must satisfy BarPolicy concept");

}  // namespace flox
