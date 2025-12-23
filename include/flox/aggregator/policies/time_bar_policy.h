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

  template <typename Rep, typename Period>
  explicit constexpr TimeBarPolicy(std::chrono::duration<Rep, Period> interval) noexcept
      : _interval(std::chrono::duration_cast<std::chrono::nanoseconds>(interval))
  {
  }

  /// Returns interval in nanoseconds
  constexpr uint64_t param() const noexcept { return _interval.count(); }

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

    const auto notional = trade.trade.quantity * trade.trade.price;
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
  TimePoint alignToInterval(TimePoint tp) const noexcept
  {
    const auto epoch = tp.time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch);
    const auto snapped = (ns.count() / _interval.count()) * _interval.count();
    return TimePoint(std::chrono::nanoseconds(snapped));
  }

  std::chrono::nanoseconds _interval;
};

static_assert(BarPolicy<TimeBarPolicy>, "TimeBarPolicy must satisfy BarPolicy concept");

}  // namespace flox
