#pragma once

#include "flox/indicator/sma.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class CCI
{
 public:
  explicit CCI(size_t period = 20) noexcept : _period(period) {}

  std::vector<double> compute(std::span<const double> high, std::span<const double> low,
                              std::span<const double> close) const
  {
    const size_t n = high.size();
    std::vector<double> out(n, std::nan(""));

    if (n < _period)
    {
      return out;
    }

    std::vector<double> tp(n);
    for (size_t i = 0; i < n; ++i)
    {
      tp[i] = (high[i] + low[i] + close[i]) / 3.0;
    }

    SMA sma(_period);
    std::vector<double> tpSma(n, std::nan(""));
    sma.compute(std::span<const double>(tp), tpSma);

    for (size_t i = _period - 1; i < n; ++i)
    {
      if (std::isnan(tpSma[i]))
      {
        continue;
      }
      double md = 0.0;
      for (size_t j = i - _period + 1; j <= i; ++j)
      {
        md += std::abs(tp[j] - tpSma[i]);
      }
      md /= static_cast<double>(_period);
      out[i] = md > 0 ? (tp[i] - tpSma[i]) / (0.015 * md) : 0.0;
    }

    return out;
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::HasPeriod<flox::indicator::CCI>);
