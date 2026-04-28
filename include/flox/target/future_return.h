#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

#include "flox/target/target.h"

namespace flox::target
{

// FutureReturn(horizon): out[t] = close[t + horizon] / close[t] - 1.
// Tail (n - horizon, ..., n - 1) is NaN.
class FutureReturn
{
 public:
  static constexpr bool is_target = true;

  explicit FutureReturn(size_t horizon) noexcept : _horizon(horizon) { assert(horizon >= 1); }

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

    for (size_t t = 0; t + _horizon < n; ++t)
    {
      double base = close[t];
      double fwd = close[t + _horizon];
      if (std::isnan(base) || std::isnan(fwd) || base == 0.0)
      {
        continue;
      }
      output[t] = fwd / base - 1.0;
    }
  }

  size_t horizon() const noexcept { return _horizon; }

 private:
  size_t _horizon;
};

static_assert(BatchTarget<FutureReturn>);

}  // namespace flox::target
