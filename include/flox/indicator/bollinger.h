#pragma once

#include "flox/indicator/sma.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

struct BollingerResult
{
  std::vector<double> upper;
  std::vector<double> middle;
  std::vector<double> lower;
};

class Bollinger
{
 public:
  explicit Bollinger(size_t period = 20, double stddev = 2.0) noexcept
      : _period(period), _stddev(stddev)
  {
  }

  BollingerResult compute(std::span<const double> input) const
  {
    const size_t n = input.size();
    BollingerResult result;
    result.upper.resize(n, std::nan(""));
    result.middle.resize(n, std::nan(""));
    result.lower.resize(n, std::nan(""));

    if (n < _period)
    {
      return result;
    }

    SMA sma(_period);
    sma.compute(input, result.middle);

    for (size_t i = _period - 1; i < n; ++i)
    {
      double mean = result.middle[i];
      double sumSq = 0.0;
      for (size_t j = i - _period + 1; j <= i; ++j)
      {
        double diff = input[j] - mean;
        sumSq += diff * diff;
      }
      double sd = std::sqrt(sumSq / static_cast<double>(_period));
      result.upper[i] = mean + _stddev * sd;
      result.lower[i] = mean - _stddev * sd;
    }

    return result;
  }

  void compute(std::span<const double> input, std::span<double> upper, std::span<double> middle,
               std::span<double> lower) const
  {
    const size_t n = input.size();
    assert(upper.size() >= n && middle.size() >= n && lower.size() >= n);

    for (size_t i = 0; i < n; ++i)
    {
      upper[i] = std::nan("");
      middle[i] = std::nan("");
      lower[i] = std::nan("");
    }

    if (n < _period)
    {
      return;
    }

    SMA sma(_period);
    sma.compute(input, middle);

    for (size_t i = _period - 1; i < n; ++i)
    {
      double mean = middle[i];
      double sumSq = 0.0;
      for (size_t j = i - _period + 1; j <= i; ++j)
      {
        double diff = input[j] - mean;
        sumSq += diff * diff;
      }
      double sd = std::sqrt(sumSq / static_cast<double>(_period));
      upper[i] = mean + _stddev * sd;
      lower[i] = mean - _stddev * sd;
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
  double _stddev;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::MultiOutputIndicator<flox::indicator::Bollinger>);
