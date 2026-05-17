/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace flox::indicator
{

// Hurst exponent via Detrended Fluctuation Analysis (DFA).
//
//   hurst_dfa(returns, scales={}) → double H
//
// Applied to a series of returns (NOT prices). Interpretation:
//   H ≈ 0.5 : returns are uncorrelated (Brownian / random walk)
//   H > 0.5 : returns are persistent (momentum / trending)
//   H < 0.5 : returns are anti-persistent (mean-reverting / choppy)
//
// Algorithm (Peng et al. 1994, Phys. Rev. E 49, 1685):
//   1. Integrated profile Y(i) = cumsum(x − mean(x)).
//   2. For each scale s, split Y into ⌊N/s⌋ non-overlapping windows of
//      length s and linearly detrend each window.
//   3. F(s) = sqrt( mean over windows of mean-squared residual ).
//   4. H = slope of log(F(s)) vs log(s) via least-squares.
//
// If `scales` is empty, defaults to a log-spaced grid from 4 to N/4
// with ~12 points. The caller may supply custom scales (e.g. shorter
// for very short series). Scales must satisfy s ≥ 4 and s ≤ N/2.
//
// Returns NaN if the series is too short (< 32) or degenerate (all
// zeros / fewer than 3 usable scales). Inherently batch — no
// streaming variant.
inline double hurst_dfa(std::span<const double> returns,
                        std::span<const int> scales = {})
{
  const auto n = static_cast<std::ptrdiff_t>(returns.size());
  if (n < 32)
  {
    return std::nan("");
  }

  // mean of x
  double mean = 0.0;
  for (auto v : returns)
  {
    mean += v;
  }
  mean /= static_cast<double>(n);

  // Integrated profile Y(i) = cumsum(x − mean)
  std::vector<double> Y(static_cast<std::size_t>(n));
  double acc = 0.0;
  for (std::ptrdiff_t i = 0; i < n; ++i)
  {
    acc += returns[static_cast<std::size_t>(i)] - mean;
    Y[static_cast<std::size_t>(i)] = acc;
  }

  // Build default scales if caller didn't supply any.
  std::vector<int> defaultScales;
  if (scales.empty())
  {
    const int lo = 4;
    const int hi = std::max(8, static_cast<int>(n / 4));
    constexpr int N_POINTS = 12;
    const double logLo = std::log(static_cast<double>(lo));
    const double logHi = std::log(static_cast<double>(hi));
    int prev = -1;
    for (int k = 0; k < N_POINTS; ++k)
    {
      const double frac = static_cast<double>(k) / static_cast<double>(N_POINTS - 1);
      const double logS = logLo + frac * (logHi - logLo);
      const int s = static_cast<int>(std::round(std::exp(logS)));
      if (s != prev && s >= 4 && s <= n / 2)
      {
        defaultScales.push_back(s);
        prev = s;
      }
    }
    scales = std::span<const int>{defaultScales};
  }

  if (scales.size() < 3)
  {
    return std::nan("");
  }

  // F(s) for each scale
  std::vector<double> logS;
  std::vector<double> logF;
  logS.reserve(scales.size());
  logF.reserve(scales.size());

  for (int s : scales)
  {
    if (s < 4 || static_cast<std::ptrdiff_t>(s) > n / 2)
    {
      continue;
    }
    const std::ptrdiff_t nWindows = n / s;
    if (nWindows < 2)
    {
      continue;
    }
    double sumSq = 0.0;
    int cnt = 0;
    // Precompute Σx, Σx², used by every window for the OLS slope.
    const double sx = static_cast<double>(s) * (static_cast<double>(s) - 1.0) / 2.0;
    double sxx = 0.0;
    for (int i = 0; i < s; ++i)
    {
      sxx += static_cast<double>(i) * static_cast<double>(i);
    }
    const double denom = static_cast<double>(s) * sxx - sx * sx;
    if (denom == 0.0)
    {
      continue;
    }

    for (std::ptrdiff_t w = 0; w < nWindows; ++w)
    {
      const std::ptrdiff_t base = w * s;
      double sy = 0.0;
      double sxy = 0.0;
      for (int i = 0; i < s; ++i)
      {
        const double yi = Y[static_cast<std::size_t>(base + i)];
        sy += yi;
        sxy += static_cast<double>(i) * yi;
      }
      const double slope = (static_cast<double>(s) * sxy - sx * sy) / denom;
      const double intercept = (sy - slope * sx) / static_cast<double>(s);

      double windowSq = 0.0;
      for (int i = 0; i < s; ++i)
      {
        const double trend = intercept + slope * static_cast<double>(i);
        const double resid = Y[static_cast<std::size_t>(base + i)] - trend;
        windowSq += resid * resid;
      }
      sumSq += windowSq / static_cast<double>(s);
      ++cnt;
    }
    if (cnt == 0)
    {
      continue;
    }
    const double F = std::sqrt(sumSq / static_cast<double>(cnt));
    if (F <= 0.0 || !std::isfinite(F))
    {
      continue;
    }
    logS.push_back(std::log(static_cast<double>(s)));
    logF.push_back(std::log(F));
  }

  if (logS.size() < 3)
  {
    return std::nan("");
  }

  // OLS slope of logF on logS — that's the Hurst exponent.
  const std::size_t m = logS.size();
  double sx2 = 0.0;
  double sy2 = 0.0;
  double sxy2 = 0.0;
  double sxx2 = 0.0;
  for (std::size_t i = 0; i < m; ++i)
  {
    sx2 += logS[i];
    sy2 += logF[i];
    sxy2 += logS[i] * logF[i];
    sxx2 += logS[i] * logS[i];
  }
  const double mD = static_cast<double>(m);
  const double slopeDen = mD * sxx2 - sx2 * sx2;
  if (slopeDen == 0.0)
  {
    return std::nan("");
  }
  return (mD * sxy2 - sx2 * sy2) / slopeDen;
}

// Rolling DFA-Hurst over price series. For each bar i, computes the
// Hurst of log-returns over the previous `window` returns (i.e.
// returns indexed [i-window, i-1] in 0-based terms). Output[i] = NaN
// for i ≤ window (insufficient history). Output has the same length
// as `prices`.
inline std::vector<double> rolling_hurst(std::span<const double> prices,
                                         std::size_t window)
{
  std::vector<double> out(prices.size(), std::nan(""));
  if (prices.size() < window + 2 || window < 32)
  {
    return out;
  }

  // log-returns are length N-1; ret[k] corresponds to bar k+1.
  const std::size_t n = prices.size();
  std::vector<double> ret(n - 1);
  for (std::size_t i = 1; i < n; ++i)
  {
    const double p0 = prices[i - 1];
    const double p1 = prices[i];
    if (p0 <= 0.0 || p1 <= 0.0)
    {
      ret[i - 1] = 0.0;
    }
    else
    {
      ret[i - 1] = std::log(p1 / p0);
    }
  }

  for (std::size_t i = window + 1; i < n; ++i)
  {
    // returns [i-window .. i-1] inclusive = ret slice [i-window-1 .. i-2]
    // (because ret[k] = log(prices[k+1]/prices[k])).
    const std::size_t lo = i - window;
    const std::size_t hi = i;  // exclusive in ret index
    auto sub = std::span<const double>(ret.data() + lo - 1, hi - lo);
    out[i] = hurst_dfa(sub);
  }
  return out;
}

}  // namespace flox::indicator
