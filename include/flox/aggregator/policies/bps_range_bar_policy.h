/*
 * Flox Engine — local extension for flox-h6 project.
 *
 * BpsRangeBarPolicy: closes a bar when (high - low) / open >= bps_threshold.
 *
 * Same as RangeBarPolicy but the threshold is RELATIVE (basis points of bar
 * open), so it works uniformly across symbols of any nominal price.
 */

#pragma once

#include "flox/aggregator/aggregation_policy.h"

namespace flox
{

class BpsRangeBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::BpsRange;

  // bpsThreshold = e.g. 20 means "close when (high - low) / open >= 20bps"
  explicit constexpr BpsRangeBarPolicy(double bpsThreshold) noexcept
      : _bps(bpsThreshold)
  {
  }

  constexpr uint64_t param() const noexcept
  {
    // Encode bps × 100 to keep an int representation if anyone reads it back
    return static_cast<uint64_t>(_bps * 100.0);
  }

  [[nodiscard]] bool shouldClose(const TradeEvent& trade, const Bar& bar) const noexcept
  {
    const auto newHigh = std::max(bar.high, trade.trade.price);
    const auto newLow = std::min(bar.low, trade.trade.price);
    if (bar.open.raw() <= 0)
    {
      return false;
    }
    // (high - low) / open in bps
    const double range = static_cast<double>(newHigh.raw() - newLow.raw());
    const double openD = static_cast<double>(bar.open.raw());
    return (range / openD) * 1e4 >= _bps;
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept
  {
    updateOHLCV(trade, bar);
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept
  {
    initBarFromTrade(trade, bar);
  }

  double bps() const noexcept { return _bps; }

 private:
  double _bps;
};

static_assert(BarPolicy<BpsRangeBarPolicy>, "BpsRangeBarPolicy must satisfy BarPolicy concept");

}  // namespace flox
