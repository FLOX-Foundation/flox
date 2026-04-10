#pragma once

#include "flox/aggregator/bar.h"
#include "flox/indicator/atr.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class CHOP
{
 public:
  explicit CHOP(size_t period) noexcept : _period(period) {}

  std::vector<double> compute(std::span<const double> high, std::span<const double> low,
                              std::span<const double> close) const
  {
    const size_t n = high.size();
    std::vector<double> out(n, std::nan(""));
    if (n < _period + 1)
    {
      return out;
    }

    ATR atr1(1);
    std::vector<double> atr1_vals(n);
    atr1.compute(high, low, close, atr1_vals);

    const double logPeriod = std::log10(static_cast<double>(_period));

    for (size_t i = _period - 1; i < n; ++i)
    {
      double atrSum = 0.0;
      size_t validCount = 0;
      for (size_t j = i - _period + 1; j <= i; ++j)
      {
        if (!std::isnan(atr1_vals[j]))
        {
          atrSum += atr1_vals[j];
          ++validCount;
        }
      }
      if (validCount < _period)
      {
        continue;
      }

      double highest = high[i - _period + 1];
      double lowest = low[i - _period + 1];
      for (size_t j = i - _period + 2; j <= i; ++j)
      {
        if (high[j] > highest)
        {
          highest = high[j];
        }
        if (low[j] < lowest)
        {
          lowest = low[j];
        }
      }

      double diff = highest - lowest;
      if (diff > 0 && atrSum > 0)
      {
        out[i] = 100.0 * std::log10(atrSum / diff) / logPeriod;
      }
    }

    return out;
  }

  std::vector<double> compute(std::span<const Bar> bars) const
  {
    const size_t n = bars.size();
    std::vector<double> h(n), l(n), c(n);
    for (size_t i = 0; i < n; ++i)
    {
      h[i] = bars[i].high.toDouble();
      l[i] = bars[i].low.toDouble();
      c[i] = bars[i].close.toDouble();
    }
    return compute(h, l, c);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::BarIndicator<flox::indicator::CHOP>);
