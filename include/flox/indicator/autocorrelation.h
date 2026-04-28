#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

// AutoCorrelation(window, lag): Pearson correlation between x[t] and x[t-lag]
// over a trailing window of `window` paired observations. Equivalent to
// Correlation(window) applied to (x[lag..], x[..-lag]) but explicit about the
// lag and warm-up.
//
//   first valid index is (window - 1 + lag).
//
// Reuses Pearson's standard closed form so output matches Correlation when
// the same data is paired manually.
class AutoCorrelation
{
 public:
  AutoCorrelation(size_t window, size_t lag) noexcept : _window(window), _lag(lag)
  {
    assert(window >= 2);
    assert(lag >= 1);
  }

  std::vector<double> compute(std::span<const double> x) const
  {
    std::vector<double> out(x.size(), std::nan(""));
    if (!x.empty())
    {
      compute(x, out);
    }
    return out;
  }

  void compute(std::span<const double> x, std::span<double> output) const
  {
    const size_t n = x.size();
    assert(output.size() >= n);

    for (size_t i = 0; i < n; ++i)
    {
      output[i] = std::nan("");
    }

    const size_t firstValid = _window + _lag - 1;
    if (n <= firstValid)
    {
      return;
    }

    const double w = static_cast<double>(_window);

    for (size_t t = firstValid; t < n; ++t)
    {
      double sx = 0, sy = 0, sxy = 0, sx2 = 0, sy2 = 0;
      for (size_t i = 0; i < _window; ++i)
      {
        double xi = x[t - _window + 1 + i];
        double yi = x[t - _window + 1 + i - _lag];
        sx += xi;
        sy += yi;
        sxy += xi * yi;
        sx2 += xi * xi;
        sy2 += yi * yi;
      }

      double num = w * sxy - sx * sy;
      double den = std::sqrt((w * sx2 - sx * sx) * (w * sy2 - sy * sy));
      if (den == 0.0)
      {
        output[t] = std::nan("");
        continue;
      }
      output[t] = num / den;
    }
  }

  size_t period() const noexcept { return _window + _lag - 1; }
  size_t window() const noexcept { return _window; }
  size_t lag() const noexcept { return _lag; }

 private:
  size_t _window;
  size_t _lag;
};

inline std::vector<double> autocorrelation(std::span<const double> x, size_t window, size_t lag)
{
  return AutoCorrelation(window, lag).compute(x);
}

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::HasPeriod<flox::indicator::AutoCorrelation>);
