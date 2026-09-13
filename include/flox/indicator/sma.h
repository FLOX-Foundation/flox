#pragma once

#include "flox/indicator/streaming.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class SMA : public StreamingSingle<SMA>
{
 public:
  explicit SMA(size_t period) noexcept : _period(period) {}

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

    // A zero period has no well-defined window; treat it like "not enough
    // data yet" instead of underflowing _period - 1 into a huge index below.
    if (_period == 0 || n < _period)
    {
      return;
    }

    // NaN in the window poisons only that window, not the rest of the
    // series: track how many of the _period most recent inputs are NaN and
    // withhold the output while that count is nonzero, excluding NaN
    // entries from the running sum so it stays finite. The window
    // recovers on its own once the NaN slides out, matching the
    // NaN-tolerant contract EMA/RSI already have (see NaNInputSkipped /
    // NaNInputHandled) instead of poisoning every output from that point on.
    double sum = 0.0;
    size_t nanCount = 0;
    for (size_t i = 0; i < _period; ++i)
    {
      if (std::isnan(input[i]))
      {
        ++nanCount;
      }
      else
      {
        sum += input[i];
      }
    }
    output[_period - 1] = nanCount == 0 ? sum / static_cast<double>(_period) : std::nan("");

    for (size_t i = _period; i < n; ++i)
    {
      const double incoming = input[i];
      const double outgoing = input[i - _period];
      if (std::isnan(incoming))
      {
        ++nanCount;
      }
      else
      {
        sum += incoming;
      }
      if (std::isnan(outgoing))
      {
        --nanCount;
      }
      else
      {
        sum -= outgoing;
      }
      output[i] = nanCount == 0 ? sum / static_cast<double>(_period) : std::nan("");
    }
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::SMA>);
