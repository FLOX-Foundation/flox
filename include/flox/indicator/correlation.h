#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class Correlation
{
 public:
  explicit Correlation(size_t period) noexcept : _period(period) { assert(period >= 2); }

  std::vector<double> compute(std::span<const double> x, std::span<const double> y) const
  {
    assert(x.size() == y.size());
    const size_t n = x.size();
    std::vector<double> out(n, std::nan(""));
    if (n > 0)
    {
      compute(x, y, out);
    }
    return out;
  }

  void compute(std::span<const double> x, std::span<const double> y,
               std::span<double> output) const
  {
    assert(x.size() == y.size());
    assert(output.size() >= x.size());
    const size_t n = x.size();

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
      double sx = 0, sy = 0, sxy = 0, sx2 = 0, sy2 = 0;
      for (size_t j = i - _period + 1; j <= i; ++j)
      {
        sx += x[j];
        sy += y[j];
        sxy += x[j] * y[j];
        sx2 += x[j] * x[j];
        sy2 += y[j] * y[j];
      }

      double num = p * sxy - sx * sy;
      double den = std::sqrt((p * sx2 - sx * sx) * (p * sy2 - sy * sy));

      if (den == 0.0)
      {
        output[i] = std::nan("");
        continue;
      }

      output[i] = num / den;
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::HasPeriod<flox::indicator::Correlation>);
