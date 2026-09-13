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

    // A zero window has no well-defined range; treat it like "not enough
    // data yet" instead of underflowing _window - 1 into a huge index below.
    if (_window == 0 || n < _window)
    {
      return out;
    }

    // NaN in the window poisons only that window, not the rest of the
    // series: a NaN close or volume is excluded from the running sums and
    // tracked in nanCount, so the output withholds a value while the
    // window is contaminated and recovers on its own once the NaN slides
    // out (see SMA's NaN handling for the same pattern).
    double pvSum = 0.0;
    double vSum = 0.0;
    size_t nanCount = 0;
    auto addBar = [&](size_t i)
    {
      if (std::isnan(close[i]) || std::isnan(volume[i]))
      {
        ++nanCount;
      }
      else
      {
        pvSum += close[i] * volume[i];
        vSum += volume[i];
      }
    };
    auto removeBar = [&](size_t i)
    {
      if (std::isnan(close[i]) || std::isnan(volume[i]))
      {
        --nanCount;
      }
      else
      {
        pvSum -= close[i] * volume[i];
        vSum -= volume[i];
      }
    };

    for (size_t i = 0; i < _window; ++i)
    {
      addBar(i);
    }
    out[_window - 1] = nanCount > 0 ? std::nan("") : (vSum > 0 ? pvSum / vSum : close[_window - 1]);

    for (size_t i = _window; i < n; ++i)
    {
      addBar(i);
      removeBar(i - _window);
      out[i] = nanCount > 0 ? std::nan("") : (vSum > 0 ? pvSum / vSum : close[i]);
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
