#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

// (input[i] - input[i-length]) / length
class Slope
{
 public:
  explicit Slope(size_t length) noexcept : _length(length) {}

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

    const double len = static_cast<double>(_length);
    for (size_t i = _length; i < n; ++i)
    {
      if (std::isnan(input[i]) || std::isnan(input[i - _length]))
      {
        continue;
      }
      output[i] = (input[i] - input[i - _length]) / len;
    }
  }

  size_t period() const noexcept { return _length; }

 private:
  size_t _length;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::Slope>);
