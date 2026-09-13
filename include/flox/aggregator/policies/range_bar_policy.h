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

  constexpr uint64_t param() const noexcept
  {
    // Same units as TimeframeId::range(rangeSize): the price range in the
    // instrument's own units (e.g. 5 for RangeBarPolicy::fromDouble(5.0)),
    // not the internal Price::Scale-scaled fixed-point _raw
    // representation. This used to truncate the raw value to 32 bits
    // instead, which both lost precision and made distinct range sizes
    // (e.g. 42.94967296 and 85.89934592) collide on the same param.
    return static_cast<uint64_t>(_rangeSizeRaw / Price::Scale);
  }

  [[nodiscard]] bool shouldClose(const TradeEvent& trade, const Bar& bar) const noexcept
  {
    // Range bar closes when the bar's own accumulated high - low already
    // reaches rangeSize. Checking against `newHigh`/`newLow` computed from
    // the *candidate* trade (as this used to) excludes that trade from the
    // closing bar, so the emitted bar's range never actually reaches the
    // threshold -- it closes one trade short every time. Checking the
    // bar's own state (already updated by the previous trade) matches how
    // every other threshold policy in this file works (Tick counts,
    // Volume sums) and means the closing trade opens the next bar, same as
    // for those policies.
    return (bar.high.raw() - bar.low.raw()) >= _rangeSizeRaw;
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
