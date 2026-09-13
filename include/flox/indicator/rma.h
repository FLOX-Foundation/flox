#pragma once

#include "flox/indicator/streaming.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

// Wilder's Moving Average. alpha = 1/period (not 2/(period+1) like EMA).
// Used internally by ATR, RSI, ADX. Exposed for direct use.
class RMA : public StreamingSingle<RMA>
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

    // A zero period has no well-defined window; treat it like "not enough
    // data yet" instead of underflowing _period - 1 into a huge index below.
    if (_period == 0 || n < _period)
    {
      return;
    }

    double alpha = 1.0 / static_cast<double>(_period);

    // Same NaN contract as EMA (RMA is Wilder's exponential average, so it
    // has the same recursive-state problem): seed on the first _period
    // consecutive non-NaN inputs, resetting the running sum whenever a NaN
    // interrupts the seed window, then hold the last output through any
    // later NaN instead of poisoning the recursion permanently.
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
