#pragma once

#include "flox/aggregator/bar.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class ATR
{
 public:
  explicit ATR(size_t period) noexcept : _period(period) {}

  std::vector<double> compute(std::span<const double> high, std::span<const double> low,
                              std::span<const double> close) const
  {
    assert(high.size() == low.size() && low.size() == close.size());
    std::vector<double> out(high.size(), std::nan(""));
    if (!high.empty())
    {
      compute(high, low, close, out);
    }
    return out;
  }

  void compute(std::span<const double> high, std::span<const double> low,
               std::span<const double> close, std::span<double> output) const
  {
    const size_t n = high.size();
    assert(output.size() >= n);
    assert(high.size() == low.size() && low.size() == close.size());

    for (size_t i = 0; i < n; ++i)
    {
      output[i] = std::nan("");
    }

    if (n < _period)
    {
      return;
    }

    // TR[0] has no prev close -- matches TA-Lib seeding from TR[1..period]
    std::vector<double> tr(n, std::nan(""));
    for (size_t i = 1; i < n; ++i)
    {
      if (std::isnan(high[i]) || std::isnan(low[i]) || std::isnan(close[i - 1]))
      {
        continue;
      }
      tr[i] = std::max({high[i] - low[i], std::abs(high[i] - close[i - 1]),
                        std::abs(low[i] - close[i - 1])});
    }

    if (n < _period + 1)
    {
      return;
    }

    double sum = 0.0;
    for (size_t i = 1; i <= _period; ++i)
    {
      sum += tr[i];
    }
    output[_period] = sum / static_cast<double>(_period);

    const double alpha = 1.0 / static_cast<double>(_period);
    for (size_t i = _period + 1; i < n; ++i)
    {
      if (std::isnan(tr[i]))
      {
        output[i] = output[i - 1];
        continue;
      }
      output[i] = alpha * tr[i] + (1.0 - alpha) * output[i - 1];
    }
  }

  std::vector<double> compute(std::span<const Bar> bars) const
  {
    std::vector<double> out(bars.size(), std::nan(""));
    if (!bars.empty())
    {
      compute(bars, out);
    }
    return out;
  }

  void compute(std::span<const Bar> bars, std::span<double> output) const
  {
    const size_t n = bars.size();
    assert(output.size() >= n);

    for (size_t i = 0; i < n; ++i)
    {
      output[i] = std::nan("");
    }

    if (n < _period)
    {
      return;
    }

    std::vector<double> tr(n, std::nan(""));
    for (size_t i = 1; i < n; ++i)
    {
      double h = bars[i].high.toDouble();
      double l = bars[i].low.toDouble();
      double pc = bars[i - 1].close.toDouble();
      tr[i] = std::max({h - l, std::abs(h - pc), std::abs(l - pc)});
    }

    if (n < _period + 1)
    {
      return;
    }

    double sum = 0.0;
    for (size_t i = 1; i <= _period; ++i)
    {
      sum += tr[i];
    }
    output[_period] = sum / static_cast<double>(_period);

    const double alpha = 1.0 / static_cast<double>(_period);
    for (size_t i = _period + 1; i < n; ++i)
    {
      output[i] = alpha * tr[i] + (1.0 - alpha) * output[i - 1];
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::BarIndicator<flox::indicator::ATR>);
