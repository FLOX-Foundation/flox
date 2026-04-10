#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class KAMA
{
 public:
  explicit KAMA(size_t period, size_t fast = 2, size_t slow = 30) noexcept
      : _period(period), _fr(2.0 / (fast + 1)), _sr(2.0 / (slow + 1))
  {
  }

  std::vector<double> compute(std::span<const double> input) const
  {
    std::vector<double> out(input.size(), std::nan(""));
    if (!input.empty())
    {
      compute(input, out);
    }
    return out;
  }

  void compute(std::span<const double> input, std::span<double> output) const
  {
    const size_t n = input.size();
    assert(output.size() >= n);

    for (size_t i = 0; i < n; ++i)
    {
      output[i] = std::nan("");
    }

    if (n <= _period)
    {
      return;
    }

    // TA-Lib seeds KAMA with close[period-1], first output at index period
    double prev = input[_period - 1];

    for (size_t i = _period; i < n; ++i)
    {
      double direction = std::abs(input[i] - input[i - _period]);
      double volatility = 0.0;
      for (size_t j = i - _period + 1; j <= i; ++j)
      {
        volatility += std::abs(input[j] - input[j - 1]);
      }
      double er = volatility > 0 ? direction / volatility : 0.0;
      double sc = (er * (_fr - _sr) + _sr);
      sc *= sc;
      prev = sc * input[i] + (1.0 - sc) * prev;
      output[i] = prev;
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
  double _fr;
  double _sr;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::KAMA>);
