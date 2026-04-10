#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class SMA
{
 public:
  explicit SMA(size_t period) noexcept : _period(period) {}

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

    double sum = 0.0;
    for (size_t i = 0; i < _period; ++i)
    {
      sum += input[i];
    }
    output[_period - 1] = sum / static_cast<double>(_period);

    for (size_t i = _period; i < n; ++i)
    {
      sum += input[i] - input[i - _period];
      output[i] = sum / static_cast<double>(_period);
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::SMA>);
