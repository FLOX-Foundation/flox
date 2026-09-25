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
    // Only a trade from a *later* interval closes the current bar. A `!=`
    // comparison also closed on a trade from an *earlier* interval (an
    // out-of-order / late-arriving trade, common when merging feeds from
    // multiple venues): that trade would prematurely close the current bar
    // one event early and then open a duplicate bucket for an interval
    // that had already closed, breaking monotonicity of bar.startTime for
    // any downstream consumer (BarSeries, BarMatrix). Such a trade is not a
    // close at all -- isLate() below says what happens to it instead.
    return alignedTradeTs > bar.startTime;
  }

  // A trade whose bucket precedes the live bar's belongs to an interval that
  // is closed and gone -- or, when the feed started mid-interval, never
  // existed. Folding it into the live bar made its price that bar's close,
  // and possibly its high or low, so the bar published for an interval
  // reported a price that never traded inside it and a trade count that
  // included a trade from another minute. The aggregators drop it and count
  // the drop (BarAggregator::lateTradeCount) rather than silently repairing
  // a bar with foreign data.
  //
  // This is about the *bucket*, not about arrival order: a trade that
  // arrives out of order but still lands in the live bucket is ordinary
  // data and is folded in like any other.
  [[nodiscard]] bool isLate(const TradeEvent& trade, const Bar& bar) const noexcept
  {
    return alignToInterval(fromUnixNs(trade.trade.exchangeTsNs)) < bar.startTime;
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
