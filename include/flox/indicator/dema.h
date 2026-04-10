#pragma once

#include "flox/indicator/ema.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class DEMA
{
 public:
  explicit DEMA(size_t period) noexcept : _ema(period), _period(period) {}

  std::vector<double> compute(std::span<const double> input) const
  {
    auto ema1 = _ema.compute(input);
    auto ema2 = _ema.compute(std::span<const double>(ema1));
    const size_t n = input.size();
    std::vector<double> out(n, std::nan(""));
    for (size_t i = 0; i < n; ++i)
    {
      if (!std::isnan(ema1[i]) && !std::isnan(ema2[i]))
      {
        out[i] = 2.0 * ema1[i] - ema2[i];
      }
    }
    return out;
  }

  size_t period() const noexcept { return _period; }

 private:
  EMA _ema;
  size_t _period;
};

class TEMA
{
 public:
  explicit TEMA(size_t period) noexcept : _ema(period), _period(period) {}

  std::vector<double> compute(std::span<const double> input) const
  {
    auto ema1 = _ema.compute(input);
    auto ema2 = _ema.compute(std::span<const double>(ema1));
    auto ema3 = _ema.compute(std::span<const double>(ema2));
    const size_t n = input.size();
    std::vector<double> out(n, std::nan(""));
    for (size_t i = 0; i < n; ++i)
    {
      if (!std::isnan(ema1[i]) && !std::isnan(ema2[i]) && !std::isnan(ema3[i]))
      {
        out[i] = 3.0 * ema1[i] - 3.0 * ema2[i] + ema3[i];
      }
    }
    return out;
  }

  size_t period() const noexcept { return _period; }

 private:
  EMA _ema;
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::DEMA>);
static_assert(flox::indicator::SingleIndicator<flox::indicator::TEMA>);
