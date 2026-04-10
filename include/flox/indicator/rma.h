#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

// Wilder's Moving Average. alpha = 1/period (not 2/(period+1) like EMA).
// Used internally by ATR, RSI, ADX. Exposed for direct use.
class RMA
{
 public:
  explicit RMA(size_t period) noexcept : _period(period) {}

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

    if (n < _period)
    {
      return;
    }

    double alpha = 1.0 / static_cast<double>(_period);

    double sum = 0.0;
    for (size_t i = 0; i < _period; ++i)
    {
      sum += input[i];
    }
    output[_period - 1] = sum / static_cast<double>(_period);

    for (size_t i = _period; i < n; ++i)
    {
      output[i] = alpha * input[i] + (1.0 - alpha) * output[i - 1];
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::RMA>);
