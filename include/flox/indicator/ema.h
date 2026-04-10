#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class EMA
{
 public:
  explicit EMA(size_t period) noexcept : _period(period), _alpha(2.0 / (period + 1.0)) {}

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

    size_t validCount = 0;
    double sum = 0.0;
    size_t seedIdx = n;  // sentinel
    for (size_t i = 0; i < n; ++i)
    {
      if (std::isnan(input[i]))
      {
        validCount = 0;
        sum = 0.0;
        continue;
      }
      sum += input[i];
      ++validCount;
      if (validCount == _period)
      {
        seedIdx = i;
        break;
      }
    }
    if (seedIdx >= n)
    {
      return;
    }

    output[seedIdx] = sum / static_cast<double>(_period);

    for (size_t i = seedIdx + 1; i < n; ++i)
    {
      if (std::isnan(input[i]))
      {
        output[i] = output[i - 1];
        continue;
      }
      output[i] = _alpha * input[i] + (1.0 - _alpha) * output[i - 1];
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
  double _alpha;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::EMA>);
