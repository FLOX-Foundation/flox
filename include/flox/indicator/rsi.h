#pragma once

#include "flox/indicator/streaming.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

namespace detail
{
// RSI value given the average gain/loss over the window. avgLoss == 0 with
// avgGain > 0 is a genuine "always up" window and reports 100. avgLoss == 0
// with avgGain == 0 too -- a fully flat window -- is the neutral midpoint
// 50, not 100: see the convention note at the call site.
inline double flatRsi(double avgGain, double avgLoss) noexcept
{
  if (avgGain == 0.0 && avgLoss == 0.0)
  {
    return 50.0;
  }
  return avgLoss > 0 ? 100.0 - 100.0 / (1.0 + avgGain / avgLoss) : 100.0;
}
}  // namespace detail

class RSI : public StreamingSingle<RSI>
{
 public:
  explicit RSI(size_t period) noexcept : _period(period) {}

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

    if (n < _period + 1)
    {
      return;
    }

    double avgGain = 0.0;
    double avgLoss = 0.0;
    for (size_t i = 1; i <= _period; ++i)
    {
      if (std::isnan(input[i]) || std::isnan(input[i - 1]))
      {
        continue;
      }
      double d = input[i] - input[i - 1];
      if (d > 0)
      {
        avgGain += d;
      }
      else
      {
        avgLoss -= d;
      }
    }
    avgGain /= static_cast<double>(_period);
    avgLoss /= static_cast<double>(_period);

    // Convention: a fully flat window (avgGain == avgLoss == 0, i.e. the seed
    // has no movement at all) reports the neutral midpoint 50, matching
    // Stochastic's flat-range convention (stochastic.h) instead of the
    // mathematical limit 100 -- which would read as "maximally overbought"
    // on an instrument that has not moved. A pure uptrend (avgLoss == 0,
    // avgGain > 0) is unaffected and still correctly reports 100.
    output[_period] = detail::flatRsi(avgGain, avgLoss);

    const double pm1 = static_cast<double>(_period - 1);
    const double pd = static_cast<double>(_period);
    for (size_t i = _period + 1; i < n; ++i)
    {
      if (std::isnan(input[i]) || std::isnan(input[i - 1]))
      {
        output[i] = output[i - 1];
        continue;
      }
      double d = input[i] - input[i - 1];
      double gain = d > 0 ? d : 0.0;
      double loss = d < 0 ? -d : 0.0;
      avgGain = (avgGain * pm1 + gain) / pd;
      avgLoss = (avgLoss * pm1 + loss) / pd;
      output[i] = detail::flatRsi(avgGain, avgLoss);
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::RSI>);
