#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class ShannonEntropy
{
 public:
  explicit ShannonEntropy(size_t period, size_t bins = 10) noexcept : _period(period), _bins(bins)
  {
    assert(period >= 2);
    assert(bins >= 2);
  }

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

    std::vector<size_t> counts(_bins);

    for (size_t i = _period - 1; i < n; ++i)
    {
      size_t start = i - _period + 1;

      double lo = input[start], hi = input[start];
      for (size_t j = start + 1; j <= i; ++j)
      {
        if (input[j] < lo)
        {
          lo = input[j];
        }
        if (input[j] > hi)
        {
          hi = input[j];
        }
      }

      if (lo == hi)
      {
        output[i] = 0.0;
        continue;
      }

      std::fill(counts.begin(), counts.end(), 0);
      double range = hi - lo;

      for (size_t j = start; j <= i; ++j)
      {
        size_t bin = static_cast<size_t>(((input[j] - lo) / range) * _bins);
        if (bin >= _bins)
        {
          bin = _bins - 1;
        }
        counts[bin]++;
      }

      double entropy = 0;
      double p_denom = static_cast<double>(_period);
      for (size_t b = 0; b < _bins; ++b)
      {
        if (counts[b] > 0)
        {
          double p = static_cast<double>(counts[b]) / p_denom;
          entropy -= p * std::log(p);
        }
      }

      output[i] = entropy / std::log(static_cast<double>(_bins));
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
  size_t _bins;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::ShannonEntropy>);
