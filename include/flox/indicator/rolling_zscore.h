#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class RollingZScore
{
 public:
  explicit RollingZScore(size_t period) noexcept : _period(period) { assert(period >= 2); }

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
    assert(output.size() >= input.size());
    const size_t n = input.size();

    for (size_t i = 0; i < n; ++i)
    {
      output[i] = std::nan("");
    }

    if (n < _period)
    {
      return;
    }

    const double p = static_cast<double>(_period);

    for (size_t i = _period - 1; i < n; ++i)
    {
      double mean = 0;
      for (size_t j = i - _period + 1; j <= i; ++j)
      {
        mean += input[j];
      }
      mean /= p;

      double sumSq = 0;
      for (size_t j = i - _period + 1; j <= i; ++j)
      {
        double d = input[j] - mean;
        sumSq += d * d;
      }

      double std = std::sqrt(sumSq / (p - 1.0));
      if (std == 0.0)
      {
        output[i] = std::nan("");
        continue;
      }

      output[i] = (input[i] - mean) / std;
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::RollingZScore>);
