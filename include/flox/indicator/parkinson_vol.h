#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

#include "flox/aggregator/bar.h"

namespace flox::indicator
{

class ParkinsonVol
{
 public:
  explicit ParkinsonVol(size_t period) noexcept : _period(period) { assert(period >= 1); }

  std::vector<double> compute(std::span<const double> high, std::span<const double> low) const
  {
    assert(high.size() == low.size());
    const size_t n = high.size();
    std::vector<double> out(n, std::nan(""));
    if (n > 0)
    {
      compute(high, low, out);
    }
    return out;
  }

  void compute(std::span<const double> high, std::span<const double> low,
               std::span<double> output) const
  {
    assert(high.size() == low.size());
    assert(output.size() >= high.size());
    const size_t n = high.size();

    for (size_t i = 0; i < n; ++i)
    {
      output[i] = std::nan("");
    }

    if (n < _period)
    {
      return;
    }

    const double inv4ln2 = 1.0 / (4.0 * std::log(2.0));

    for (size_t i = _period - 1; i < n; ++i)
    {
      double sum = 0;
      bool valid = true;
      for (size_t j = i - _period + 1; j <= i; ++j)
      {
        if (low[j] <= 0.0 || high[j] < low[j])
        {
          valid = false;
          break;
        }
        double lnhl = std::log(high[j] / low[j]);
        sum += lnhl * lnhl;
      }

      if (!valid)
      {
        continue;
      }

      output[i] = std::sqrt(sum / static_cast<double>(_period) * inv4ln2);
    }
  }

  std::vector<double> compute(std::span<const Bar> bars) const
  {
    const size_t n = bars.size();
    std::vector<double> h(n), l(n);
    for (size_t i = 0; i < n; ++i)
    {
      h[i] = bars[i].high.toDouble();
      l[i] = bars[i].low.toDouble();
    }
    return compute(std::span<const double>(h), std::span<const double>(l));
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::HasPeriod<flox::indicator::ParkinsonVol>);
