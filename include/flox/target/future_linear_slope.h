#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

#include "flox/target/target.h"

namespace flox::target
{

// FutureLinearSlope(horizon): OLS slope of close over the points
// (i, close[t + i]) for i in [0, horizon]. Uses (horizon + 1) points.
// Returns NaN where the future window is unavailable.
//
// horizon must be >= 1 (need at least two points for a slope).
//
// Closed form for x = 0..horizon:
//   slope = (n * sum(x*y) - sum(x) * sum(y)) / (n * sum(x*x) - sum(x)^2)
class FutureLinearSlope
{
 public:
  static constexpr bool is_target = true;

  explicit FutureLinearSlope(size_t horizon) noexcept : _horizon(horizon)
  {
    assert(horizon >= 1);
  }

  std::vector<double> compute(std::span<const double> close) const
  {
    std::vector<double> out(close.size(), std::nan(""));
    if (!close.empty())
    {
      compute(close, out);
    }
    return out;
  }

  void compute(std::span<const double> close, std::span<double> output) const
  {
    const size_t n = close.size();
    assert(output.size() >= n);

    for (size_t i = 0; i < n; ++i)
    {
      output[i] = std::nan("");
    }

    if (n <= _horizon)
    {
      return;
    }

    const size_t points = _horizon + 1;
    const double np = static_cast<double>(points);
    // sum(x), sum(x*x) for x = 0..horizon are constants.
    const double sumX = static_cast<double>(_horizon) * np / 2.0;
    const double sumX2 = static_cast<double>(_horizon) * np * (2.0 * _horizon + 1.0) / 6.0;
    const double denom = np * sumX2 - sumX * sumX;
    if (denom == 0.0)
    {
      return;
    }

    for (size_t t = 0; t + _horizon < n; ++t)
    {
      double sumY = 0.0;
      double sumXY = 0.0;
      bool valid = true;

      for (size_t i = 0; i < points; ++i)
      {
        double y = close[t + i];
        if (std::isnan(y))
        {
          valid = false;
          break;
        }
        sumY += y;
        sumXY += static_cast<double>(i) * y;
      }

      if (!valid)
      {
        continue;
      }

      output[t] = (np * sumXY - sumX * sumY) / denom;
    }
  }

  size_t horizon() const noexcept { return _horizon; }

 private:
  size_t _horizon;
};

static_assert(BatchTarget<FutureLinearSlope>);

}  // namespace flox::target
