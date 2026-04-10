#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class OBV
{
 public:
  std::vector<double> compute(std::span<const double> close, std::span<const double> volume) const
  {
    assert(close.size() == volume.size());
    const size_t n = close.size();
    std::vector<double> out(n, std::nan(""));
    if (n == 0)
    {
      return out;
    }
    compute(close, volume, out);
    return out;
  }

  void compute(std::span<const double> close, std::span<const double> volume,
               std::span<double> output) const
  {
    const size_t n = close.size();
    assert(output.size() >= n);

    if (n == 0)
    {
      return;
    }

    output[0] = volume[0];  // TA-Lib convention
    double cumulative = volume[0];
    for (size_t i = 1; i < n; ++i)
    {
      if (std::isnan(close[i]) || std::isnan(close[i - 1]))
      {
        output[i] = cumulative;
        continue;
      }
      if (close[i] > close[i - 1])
      {
        cumulative += volume[i];
      }
      else if (close[i] < close[i - 1])
      {
        cumulative -= volume[i];
      }
      output[i] = cumulative;
    }
  }

  size_t period() const noexcept { return 1; }
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::HasPeriod<flox::indicator::OBV>);
