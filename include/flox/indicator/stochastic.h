#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

struct StochasticResult
{
  std::vector<double> k;
  std::vector<double> d;
};

class Stochastic
{
 public:
  Stochastic(size_t kPeriod = 14, size_t dPeriod = 3) noexcept
      : _kPeriod(kPeriod), _dPeriod(dPeriod)
  {
  }

  StochasticResult compute(std::span<const double> high, std::span<const double> low,
                           std::span<const double> close) const
  {
    const size_t n = high.size();
    StochasticResult result;
    result.k.resize(n, std::nan(""));
    result.d.resize(n, std::nan(""));

    if (n < _kPeriod)
    {
      return result;
    }

    // TA-Lib aligns K and D output to the same start index
    size_t firstValid = _kPeriod - 1 + _dPeriod - 1;
    size_t kStart = firstValid >= _dPeriod - 1 ? firstValid - _dPeriod + 1 : 0;
    std::vector<double> rawK(n, std::nan(""));
    for (size_t i = std::max(kStart, _kPeriod - 1); i < n; ++i)
    {
      double hh = high[i];
      double ll = low[i];
      for (size_t j = i - _kPeriod + 1; j < i; ++j)
      {
        if (high[j] > hh)
        {
          hh = high[j];
        }
        if (low[j] < ll)
        {
          ll = low[j];
        }
      }
      double range = hh - ll;
      rawK[i] = range > 0 ? 100.0 * (close[i] - ll) / range : 50.0;
    }

    for (size_t i = firstValid; i < n; ++i)
    {
      result.k[i] = rawK[i];
      double sum = 0.0;
      for (size_t j = i - _dPeriod + 1; j <= i; ++j)
      {
        sum += rawK[j];
      }
      result.d[i] = sum / static_cast<double>(_dPeriod);
    }

    return result;
  }

  size_t period() const noexcept { return _kPeriod; }

 private:
  size_t _kPeriod;
  size_t _dPeriod;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::HasPeriod<flox::indicator::Stochastic>);
