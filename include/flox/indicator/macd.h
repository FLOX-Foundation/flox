#pragma once

#include "flox/indicator/ema.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

struct MacdResult
{
  std::vector<double> line;
  std::vector<double> signal;
  std::vector<double> histogram;
};

class MACD
{
 public:
  MACD(size_t fastPeriod = 12, size_t slowPeriod = 26, size_t signalPeriod = 9) noexcept
      : _fast(fastPeriod), _slow(slowPeriod), _signal(signalPeriod), _slowPeriod(slowPeriod)
  {
  }

  MacdResult compute(std::span<const double> input) const
  {
    MacdResult result;
    result.line.resize(input.size(), std::nan(""));
    result.signal.resize(input.size(), std::nan(""));
    result.histogram.resize(input.size(), std::nan(""));
    compute(input, result.line, result.signal, result.histogram);
    return result;
  }

  void compute(std::span<const double> input, std::span<double> line, std::span<double> signal,
               std::span<double> histogram) const
  {
    const size_t n = input.size();
    assert(line.size() >= n && signal.size() >= n && histogram.size() >= n);

    // TA-Lib MACD seeds fast EMA at slow EMA start point, not independently.
    // Fast EMA SMA seed = mean(input[slowPeriod-fastPeriod .. slowPeriod-1])
    size_t fastP = _fast.period();
    size_t slowP = _slow.period();
    size_t sigP = _signal.period();

    // Ensure slow >= fast (swap if needed, matching TA-Lib behavior)
    if (fastP > slowP)
    {
      std::swap(fastP, slowP);
    }

    auto slowEma = EMA(slowP).compute(input);

    std::vector<double> fastEma(n, std::nan(""));
    if (n >= slowP)
    {
      double alpha = 2.0 / (static_cast<double>(fastP) + 1.0);
      double sum = 0.0;
      for (size_t i = slowP - fastP; i < slowP; ++i)
      {
        sum += input[i];
      }
      fastEma[slowP - 1] = sum / static_cast<double>(fastP);
      for (size_t i = slowP; i < n; ++i)
      {
        fastEma[i] = alpha * input[i] + (1.0 - alpha) * fastEma[i - 1];
      }
    }

    std::vector<double> macdLine(n, std::nan(""));
    for (size_t i = 0; i < n; ++i)
    {
      if (!std::isnan(fastEma[i]) && !std::isnan(slowEma[i]))
      {
        macdLine[i] = fastEma[i] - slowEma[i];
      }
    }

    auto signalEma = _signal.compute(std::span<const double>(macdLine));

    // TA-Lib convention: all three outputs start at the same index
    // (when signal is first valid). Line and histogram are NaN before that.
    for (size_t i = 0; i < n; ++i)
    {
      if (!std::isnan(signalEma[i]))
      {
        line[i] = macdLine[i];
        signal[i] = signalEma[i];
        histogram[i] = macdLine[i] - signalEma[i];
      }
      else
      {
        line[i] = std::nan("");
        signal[i] = std::nan("");
        histogram[i] = std::nan("");
      }
    }
  }

  size_t period() const noexcept { return _slowPeriod + _signal.period() - 1; }

 private:
  EMA _fast;
  EMA _slow;
  EMA _signal;
  size_t _slowPeriod;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::MultiOutputIndicator<flox::indicator::MACD>);
