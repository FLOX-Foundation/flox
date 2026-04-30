#pragma once

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

struct StochasticResult
{
  std::vector<double> k;
  std::vector<double> d;
};

class Stochastic
{
 public:
  Stochastic(size_t kPeriod = 14, size_t dPeriod = 3) noexcept
      : _kPeriod(kPeriod), _dPeriod(dPeriod)
  {
  }

  // ── Streaming API ─────────────────────────────────────────────────
  void update(double high, double low, double close)
  {
    _high.push_back(high);
    _low.push_back(low);
    _close.push_back(close);
    _dirty = true;
  }
  double value() const { return tail(_last.k); }
  double kValue() const { return tail(_last.k); }
  double dValue() const { return tail(_last.d); }
  bool ready() const
  {
    refresh();
    return !_last.k.empty() && std::isfinite(_last.k.back()) &&
           !_last.d.empty() && std::isfinite(_last.d.back());
  }
  void reset()
  {
    _high.clear();
    _low.clear();
    _close.clear();
    _last = StochasticResult{};
    _dirty = false;
  }
  size_t count() const noexcept { return _high.size(); }

  StochasticResult compute(std::span<const double> high, std::span<const double> low,
                           std::span<const double> close) const
  {
    const size_t n = high.size();
    StochasticResult result;
    result.k.resize(n, std::nan(""));
    result.d.resize(n, std::nan(""));

    if (n < _kPeriod)
    {
      return result;
    }

    // TA-Lib aligns K and D output to the same start index
    size_t firstValid = _kPeriod - 1 + _dPeriod - 1;
    size_t kStart = firstValid >= _dPeriod - 1 ? firstValid - _dPeriod + 1 : 0;
    std::vector<double> rawK(n, std::nan(""));
    for (size_t i = std::max(kStart, _kPeriod - 1); i < n; ++i)
    {
      double hh = high[i];
      double ll = low[i];
      for (size_t j = i - _kPeriod + 1; j < i; ++j)
      {
        if (high[j] > hh)
        {
          hh = high[j];
        }
        if (low[j] < ll)
        {
          ll = low[j];
        }
      }
      double range = hh - ll;
      rawK[i] = range > 0 ? 100.0 * (close[i] - ll) / range : 50.0;
    }

    for (size_t i = firstValid; i < n; ++i)
    {
      result.k[i] = rawK[i];
      double sum = 0.0;
      for (size_t j = i - _dPeriod + 1; j <= i; ++j)
      {
        sum += rawK[j];
      }
      result.d[i] = sum / static_cast<double>(_dPeriod);
    }

    return result;
  }

  size_t period() const noexcept { return _kPeriod; }

 private:
  void refresh() const
  {
    if (!_dirty)
    {
      return;
    }
    if (_high.empty())
    {
      _last = StochasticResult{};
    }
    else
    {
      _last = compute(std::span<const double>(_high), std::span<const double>(_low),
                      std::span<const double>(_close));
    }
    _dirty = false;
  }
  double tail(const std::vector<double>& v) const
  {
    refresh();
    return v.empty() ? std::nan("") : v.back();
  }

  size_t _kPeriod;
  size_t _dPeriod;

  // streaming state
  std::vector<double> _high, _low, _close;
  mutable StochasticResult _last;
  mutable bool _dirty = false;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::HasPeriod<flox::indicator::Stochastic>);
