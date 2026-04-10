#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class CVD
{
 public:
  std::vector<double> compute(std::span<const double> open, std::span<const double> high,
                              std::span<const double> low, std::span<const double> close,
                              std::span<const double> volume) const
  {
    const size_t n = close.size();
    std::vector<double> out(n);

    double cumulative = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
      double range = high[i] - low[i];
      double delta = range > 0 ? volume[i] * (close[i] - open[i]) / range : 0.0;
      cumulative += delta;
      out[i] = cumulative;
    }

    return out;
  }

  std::vector<double> compute(std::span<const double> volume,
                              std::span<const double> takerBuyVolume) const
  {
    const size_t n = volume.size();
    std::vector<double> out(n);

    double cumulative = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
      double delta = 2.0 * takerBuyVolume[i] - volume[i];
      cumulative += delta;
      out[i] = cumulative;
    }

    return out;
  }

  size_t period() const noexcept { return 1; }
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::HasPeriod<flox::indicator::CVD>);
