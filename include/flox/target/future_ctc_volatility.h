#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

#include "flox/target/target.h"

namespace flox::target
{

// FutureCTCVolatility(horizon): close-to-close realised volatility over
// [t, t + horizon]. Computed as the sample standard deviation of the
// `horizon` close-to-close log returns r_i = ln(close[t+i+1] / close[t+i])
// for i in [0, horizon). Uses the `(n-1)` denominator (sample stddev).
//
// horizon must be >= 2 (need at least two log-returns for sample stddev).
//
// Tail (n - horizon, ..., n - 1) is NaN.
class FutureCTCVolatility
{
 public:
  static constexpr bool is_target = true;

  explicit FutureCTCVolatility(size_t horizon) noexcept : _horizon(horizon)
  {
    assert(horizon >= 2);
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

    const double denom = static_cast<double>(_horizon - 1);

    for (size_t t = 0; t + _horizon < n; ++t)
    {
      double sum = 0.0;
      double sumSq = 0.0;
      bool valid = true;

      for (size_t i = 0; i < _horizon; ++i)
      {
        double a = close[t + i];
        double b = close[t + i + 1];
        if (std::isnan(a) || std::isnan(b) || a <= 0.0 || b <= 0.0)
        {
          valid = false;
          break;
        }
        double r = std::log(b / a);
        sum += r;
        sumSq += r * r;
      }

      if (!valid)
      {
        continue;
      }

      double mean = sum / static_cast<double>(_horizon);
      double var = (sumSq - static_cast<double>(_horizon) * mean * mean) / denom;
      if (var < 0.0)
      {
        var = 0.0;
      }
      output[t] = std::sqrt(var);
    }
  }

  size_t horizon() const noexcept { return _horizon; }

 private:
  size_t _horizon;
};

static_assert(BatchTarget<FutureCTCVolatility>);

}  // namespace flox::target
