#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

#include "flox/aggregator/bar.h"

namespace flox::indicator
{

class RogersSatchellVol
{
 public:
  explicit RogersSatchellVol(size_t period) noexcept : _period(period) { assert(period >= 1); }

  std::vector<double> compute(std::span<const double> open, std::span<const double> high,
                              std::span<const double> low, std::span<const double> close) const
  {
    assert(open.size() == high.size() && high.size() == low.size() && low.size() == close.size());
    const size_t n = open.size();
    std::vector<double> out(n, std::nan(""));
    if (n > 0)
    {
      compute(open, high, low, close, out);
    }
    return out;
  }

  void compute(std::span<const double> open, std::span<const double> high,
               std::span<const double> low, std::span<const double> close,
               std::span<double> output) const
  {
    assert(open.size() == high.size() && high.size() == low.size() && low.size() == close.size());
    assert(output.size() >= open.size());
    const size_t n = open.size();

    for (size_t i = 0; i < n; ++i)
    {
      output[i] = std::nan("");
    }

    if (n < _period)
    {
      return;
    }

    for (size_t i = _period - 1; i < n; ++i)
    {
      double sum = 0;
      bool valid = true;
      for (size_t j = i - _period + 1; j <= i; ++j)
      {
        if (open[j] <= 0.0 || close[j] <= 0.0)
        {
          valid = false;
          break;
        }
        double lnHC = std::log(high[j] / close[j]);
        double lnHO = std::log(high[j] / open[j]);
        double lnLC = std::log(low[j] / close[j]);
        double lnLO = std::log(low[j] / open[j]);
        sum += lnHC * lnHO + lnLC * lnLO;
      }

      if (!valid)
      {
        continue;
      }

      double avg = sum / static_cast<double>(_period);
      output[i] = avg >= 0.0 ? std::sqrt(avg) : 0.0;
    }
  }

  std::vector<double> compute(std::span<const Bar> bars) const
  {
    const size_t n = bars.size();
    std::vector<double> o(n), h(n), l(n), c(n);
    for (size_t i = 0; i < n; ++i)
    {
      o[i] = bars[i].open.toDouble();
      h[i] = bars[i].high.toDouble();
      l[i] = bars[i].low.toDouble();
      c[i] = bars[i].close.toDouble();
    }
    return compute(std::span<const double>(o), std::span<const double>(h),
                   std::span<const double>(l), std::span<const double>(c));
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::BarIndicator<flox::indicator::RogersSatchellVol>);
