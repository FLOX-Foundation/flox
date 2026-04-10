#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

// Rolling VWAP: sum(price * volume, window) / sum(volume, window)
class VWAP
{
 public:
  explicit VWAP(size_t window) noexcept : _window(window) {}

  std::vector<double> compute(std::span<const double> close,
                              std::span<const double> volume) const
  {
    assert(close.size() == volume.size());
    const size_t n = close.size();
    std::vector<double> out(n, std::nan(""));

    if (n < _window)
    {
      return out;
    }

    double pvSum = 0.0;
    double vSum = 0.0;
    for (size_t i = 0; i < _window; ++i)
    {
      pvSum += close[i] * volume[i];
      vSum += volume[i];
    }
    out[_window - 1] = vSum > 0 ? pvSum / vSum : close[_window - 1];

    for (size_t i = _window; i < n; ++i)
    {
      pvSum += close[i] * volume[i] - close[i - _window] * volume[i - _window];
      vSum += volume[i] - volume[i - _window];
      out[i] = vSum > 0 ? pvSum / vSum : close[i];
    }

    return out;
  }

  size_t period() const noexcept { return _window; }

 private:
  size_t _window;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::HasPeriod<flox::indicator::VWAP>);
