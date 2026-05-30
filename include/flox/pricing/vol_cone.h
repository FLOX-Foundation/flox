#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// Volatility cone: realized-vol percentiles by horizon, the backdrop a trader
// reads implied against. For each horizon the cone slides a window across the
// return history, annualizes the realized vol in each window, and reports its
// distribution (min / 25 / 50 / 75 / max). Today's implied plotted on the cone
// tells you whether options are rich or cheap versus how the underlying has
// actually moved.

namespace flox::pricing
{

// Annualized realized volatility from a window of (log) returns: the sample
// standard deviation scaled by sqrt(periodsPerYear). Fewer than 2 points -> 0.
inline double realizedVol(const std::vector<double>& returns, double periodsPerYear)
{
  const size_t n = returns.size();
  if (n < 2)
  {
    return 0.0;
  }
  double mean = 0.0;
  for (double r : returns)
  {
    mean += r;
  }
  mean /= static_cast<double>(n);
  double sse = 0.0;
  for (double r : returns)
  {
    const double d = r - mean;
    sse += d * d;
  }
  const double variance = sse / static_cast<double>(n - 1);  // sample variance
  return std::sqrt(variance * periodsPerYear);
}

// Close-to-close log returns from a price series.
inline std::vector<double> logReturns(const std::vector<double>& prices)
{
  std::vector<double> r;
  if (prices.size() < 2)
  {
    return r;
  }
  r.reserve(prices.size() - 1);
  for (size_t i = 1; i < prices.size(); ++i)
  {
    if (prices[i] > 0.0 && prices[i - 1] > 0.0)
    {
      r.push_back(std::log(prices[i] / prices[i - 1]));
    }
  }
  return r;
}

// Every rolling-window realized vol across the return series: window `window`
// returns per sample, annualized. Empty when the series is shorter than window.
inline std::vector<double> rollingRealizedVols(const std::vector<double>& returns, size_t window,
                                               double periodsPerYear)
{
  std::vector<double> out;
  if (window < 2 || returns.size() < window)
  {
    return out;
  }
  for (size_t end = window; end <= returns.size(); ++end)
  {
    const std::vector<double> slice(returns.begin() + static_cast<long>(end - window),
                                    returns.begin() + static_cast<long>(end));
    out.push_back(realizedVol(slice, periodsPerYear));
  }
  return out;
}

struct ConePercentiles
{
  size_t horizon{0};  // window length in periods
  double min{0.0};
  double p25{0.0};
  double p50{0.0};
  double p75{0.0};
  double max{0.0};
  size_t samples{0};
};

// Linear-interpolated percentile of a sorted sample (q in [0, 1]).
inline double percentileOfSorted(const std::vector<double>& sorted, double q)
{
  if (sorted.empty())
  {
    return 0.0;
  }
  if (sorted.size() == 1)
  {
    return sorted.front();
  }
  const double pos = q * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(pos));
  const size_t hi = static_cast<size_t>(std::ceil(pos));
  const double frac = pos - static_cast<double>(lo);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

// Build the cone over a set of horizons (in return-periods) from a price series.
inline std::vector<ConePercentiles> volCone(const std::vector<double>& prices,
                                            const std::vector<size_t>& horizons,
                                            double periodsPerYear)
{
  const std::vector<double> returns = logReturns(prices);
  std::vector<ConePercentiles> cone;
  cone.reserve(horizons.size());
  for (size_t h : horizons)
  {
    std::vector<double> rv = rollingRealizedVols(returns, h, periodsPerYear);
    ConePercentiles cp;
    cp.horizon = h;
    cp.samples = rv.size();
    if (!rv.empty())
    {
      std::sort(rv.begin(), rv.end());
      cp.min = rv.front();
      cp.max = rv.back();
      cp.p25 = percentileOfSorted(rv, 0.25);
      cp.p50 = percentileOfSorted(rv, 0.50);
      cp.p75 = percentileOfSorted(rv, 0.75);
    }
    cone.push_back(cp);
  }
  return cone;
}

// Where today's implied vol sits in the realized cone for one horizon: the
// fraction of historical realized-vol observations at or below `impliedVol`
// (0..1). High = options rich vs realized history; low = cheap. Empty samples
// return NaN.
inline double impliedPercentileInCone(const std::vector<double>& realizedSamples, double impliedVol)
{
  if (realizedSamples.empty())
  {
    return std::nan("");
  }
  size_t atOrBelow = 0;
  for (double v : realizedSamples)
  {
    if (v <= impliedVol)
    {
      ++atOrBelow;
    }
  }
  return static_cast<double>(atOrBelow) / static_cast<double>(realizedSamples.size());
}

}  // namespace flox::pricing
