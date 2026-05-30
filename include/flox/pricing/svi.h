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
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// SVI (Stochastic Volatility Inspired) implied-volatility surface. A backtest
// that reprices options at a single flat vol is a toy; a serious one marks each
// step to the surface as it was on that date. This builds that surface:
//
//   - A raw-SVI slice fits one expiry's smile with five parameters, calibrated
//     by least squares to observed (log-moneyness, total-variance) points.
//   - A butterfly-arbitrage check (Gatheral's g(k) >= 0) flags a slice whose
//     fit would imply a negative risk-neutral density.
//   - A surface stacks slices across expiries, interpolates in total-variance
//     space, and checks calendar arbitrage (total variance non-decreasing in
//     time). A point-in-time build uses only quotes timestamped on or before an
//     as-of date, so a backtest never sees a surface from the future.
//
// Raw-SVI total variance (Gatheral): for log-moneyness k = ln(K / forward),
//   w(k) = a + b * ( rho * (k - m) + sqrt((k - m)^2 + sigma^2) )
// and the Black-Scholes implied vol is sqrt(w(k) / t).

namespace flox::pricing
{

struct SVIParams
{
  double a{0.0};      // vertical shift (minimum total variance level)
  double b{0.0};      // wing slope, b >= 0
  double rho{0.0};    // skew, -1 < rho < 1
  double m{0.0};      // horizontal shift of the smile minimum
  double sigma{0.1};  // smoothness at the money, sigma > 0

  // Total implied variance at log-moneyness k.
  double totalVariance(double k) const
  {
    const double km = k - m;
    return a + b * (rho * km + std::sqrt(km * km + sigma * sigma));
  }

  // First derivative dw/dk.
  double dwdk(double k) const
  {
    const double km = k - m;
    return b * (rho + km / std::sqrt(km * km + sigma * sigma));
  }

  // Second derivative d2w/dk2.
  double d2wdk2(double k) const
  {
    const double km = k - m;
    const double r = km * km + sigma * sigma;
    return b * sigma * sigma / (r * std::sqrt(r));
  }

  // Black-Scholes implied vol implied by this slice at expiry t (years).
  double impliedVol(double k, double t) const
  {
    const double w = totalVariance(k);
    return (w <= 0.0 || t <= 0.0) ? 0.0 : std::sqrt(w / t);
  }
};

// Gatheral's g(k): a slice is free of butterfly arbitrage iff g(k) >= 0 for all
// k. A negative value means the implied risk-neutral density goes negative.
inline double sviDensityG(const SVIParams& p, double k)
{
  const double w = p.totalVariance(k);
  if (w <= 0.0)
  {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double wp = p.dwdk(k);
  const double wpp = p.d2wdk2(k);
  const double term1 = 1.0 - k * wp / (2.0 * w);
  const double term2 = (wp * wp / 4.0) * (1.0 / w + 0.25);
  return term1 * term1 - term2 + wpp / 2.0;
}

// True when the slice is butterfly-arbitrage-free across [kLo, kHi].
inline bool isButterflyFree(const SVIParams& p, double kLo = -1.5, double kHi = 1.5,
                            int samples = 100)
{
  for (int i = 0; i <= samples; ++i)
  {
    const double k = kLo + (kHi - kLo) * (static_cast<double>(i) / samples);
    if (sviDensityG(p, k) < -1e-9)
    {
      return false;
    }
  }
  return true;
}

namespace detail
{

// Sum of squared errors of a raw-SVI fit against observed (k, w) points.
inline double sviSSE(const SVIParams& p, const std::vector<double>& k,
                     const std::vector<double>& w)
{
  double sse = 0.0;
  for (size_t i = 0; i < k.size(); ++i)
  {
    const double d = p.totalVariance(k[i]) - w[i];
    sse += d * d;
  }
  return sse;
}

// Map a 5-vector of unconstrained reals to SVI params, enforcing b>=0,
// sigma>0, |rho|<1 via smooth transforms so the optimizer can roam freely.
inline SVIParams fromUnconstrained(const std::array<double, 5>& x)
{
  SVIParams p;
  p.a = x[0];
  p.b = std::exp(x[1]);
  p.rho = std::tanh(x[2]);
  p.m = x[3];
  p.sigma = std::exp(x[4]);
  return p;
}

// Nelder-Mead simplex minimization of the SVI SSE in unconstrained space.
inline std::array<double, 5> nelderMeadSVI(std::array<double, 5> x0,
                                           const std::vector<double>& k,
                                           const std::vector<double>& w, int maxIter)
{
  constexpr int N = 5;
  auto f = [&](const std::array<double, 5>& x)
  { return sviSSE(fromUnconstrained(x), k, w); };

  std::array<std::array<double, 5>, N + 1> simplex;
  std::array<double, N + 1> fval;
  simplex[0] = x0;
  for (int i = 0; i < N; ++i)
  {
    std::array<double, 5> xi = x0;
    xi[i] += (xi[i] != 0.0 ? 0.05 * std::fabs(xi[i]) : 0.05) + 0.05;
    simplex[i + 1] = xi;
  }
  for (int i = 0; i <= N; ++i)
  {
    fval[i] = f(simplex[i]);
  }

  const double alpha = 1.0, gamma = 2.0, rho = 0.5, sigmaShrink = 0.5;
  for (int iter = 0; iter < maxIter; ++iter)
  {
    std::array<int, N + 1> order;
    for (int i = 0; i <= N; ++i)
    {
      order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](int a, int b)
              { return fval[a] < fval[b]; });

    const int best = order[0];
    const int worst = order[N];
    const int secondWorst = order[N - 1];
    if (std::fabs(fval[worst] - fval[best]) < 1e-14)
    {
      break;
    }

    std::array<double, 5> centroid{};
    for (int i = 0; i <= N; ++i)
    {
      if (i == worst)
      {
        continue;
      }
      for (int j = 0; j < N; ++j)
      {
        centroid[j] += simplex[i][j] / N;
      }
    }

    auto reflectPoint = [&](double coef)
    {
      std::array<double, 5> p;
      for (int j = 0; j < N; ++j)
      {
        p[j] = centroid[j] + coef * (centroid[j] - simplex[worst][j]);
      }
      return p;
    };

    const std::array<double, 5> xr = reflectPoint(alpha);
    const double fr = f(xr);
    if (fr < fval[best])
    {
      const std::array<double, 5> xe = reflectPoint(alpha * gamma);
      const double fe = f(xe);
      if (fe < fr)
      {
        simplex[worst] = xe;
        fval[worst] = fe;
      }
      else
      {
        simplex[worst] = xr;
        fval[worst] = fr;
      }
    }
    else if (fr < fval[secondWorst])
    {
      simplex[worst] = xr;
      fval[worst] = fr;
    }
    else
    {
      const std::array<double, 5> xc = reflectPoint(rho);
      const double fc = f(xc);
      if (fc < fval[worst])
      {
        simplex[worst] = xc;
        fval[worst] = fc;
      }
      else
      {
        for (int i = 0; i <= N; ++i)
        {
          if (i == best)
          {
            continue;
          }
          for (int j = 0; j < N; ++j)
          {
            simplex[i][j] = simplex[best][j] + sigmaShrink * (simplex[i][j] - simplex[best][j]);
          }
          fval[i] = f(simplex[i]);
        }
      }
    }
  }

  int best = 0;
  for (int i = 1; i <= N; ++i)
  {
    if (fval[i] < fval[best])
    {
      best = i;
    }
  }
  return simplex[best];
}

}  // namespace detail

// Calibrate a raw-SVI slice to observed (log-moneyness, total-variance) points
// by least squares. Needs at least 5 points to identify the 5 parameters.
inline SVIParams calibrateSVI(const std::vector<double>& k, const std::vector<double>& w,
                              int maxIter = 2000)
{
  SVIParams seed;
  if (k.size() < 5)
  {
    return seed;  // under-determined; caller should check input size
  }

  const double wMin = *std::min_element(w.begin(), w.end());
  // Seed: minimum variance level for a, flat-ish wings, mild negative skew.
  std::array<double, 5> x0{wMin > 0.0 ? wMin : 1e-4, std::log(0.1), std::atanh(-0.3 + 1e-9), 0.0,
                           std::log(0.1)};
  const auto xOpt = detail::nelderMeadSVI(x0, k, w, maxIter);
  return detail::fromUnconstrained(xOpt);
}

// One calibrated expiry slice.
struct VolSliceFit
{
  double t{0.0};  // years to expiry
  SVIParams params;
};

// A term structure of SVI slices. Interpolates implied vol at any (k, t):
// raw-SVI within a slice, linear-in-total-variance across slices, time-scaled
// flat extrapolation beyond the first / last expiry.
class VolSurface
{
 public:
  // Slices must be added in any order; they are kept sorted by expiry.
  void addSlice(double t, const SVIParams& p)
  {
    _slices.push_back({t, p});
    std::sort(_slices.begin(), _slices.end(),
              [](const VolSliceFit& l, const VolSliceFit& r)
              { return l.t < r.t; });
  }

  size_t sliceCount() const { return _slices.size(); }
  const std::vector<VolSliceFit>& slices() const { return _slices; }

  // Total implied variance at (k, t). Empty surface returns 0.
  double totalVariance(double k, double t) const
  {
    if (_slices.empty() || t <= 0.0)
    {
      return 0.0;
    }
    if (t <= _slices.front().t)
    {
      // Flat-vol extrapolation: variance proportional to time.
      const auto& s = _slices.front();
      return s.params.totalVariance(k) * (t / s.t);
    }
    if (t >= _slices.back().t)
    {
      const auto& s = _slices.back();
      return s.params.totalVariance(k) * (t / s.t);
    }
    // Linear in total variance between the bracketing slices.
    for (size_t i = 1; i < _slices.size(); ++i)
    {
      if (t <= _slices[i].t)
      {
        const auto& lo = _slices[i - 1];
        const auto& hi = _slices[i];
        const double wLo = lo.params.totalVariance(k);
        const double wHi = hi.params.totalVariance(k);
        const double frac = (t - lo.t) / (hi.t - lo.t);
        return wLo + (wHi - wLo) * frac;
      }
    }
    return _slices.back().params.totalVariance(k);
  }

  // Black-Scholes implied vol at (k, t) — the value a backtest marks to.
  double impliedVol(double k, double t) const
  {
    const double w = totalVariance(k, t);
    return (w <= 0.0 || t <= 0.0) ? 0.0 : std::sqrt(w / t);
  }

  // Calendar-arbitrage-free iff total variance is non-decreasing in time at
  // every sampled log-moneyness. Checked on the calibrated slices.
  bool isCalendarFree(double kLo = -1.5, double kHi = 1.5, int samples = 50) const
  {
    for (int s = 0; s <= samples; ++s)
    {
      const double k = kLo + (kHi - kLo) * (static_cast<double>(s) / samples);
      for (size_t i = 1; i < _slices.size(); ++i)
      {
        if (_slices[i].params.totalVariance(k) < _slices[i - 1].params.totalVariance(k) - 1e-9)
        {
          return false;
        }
      }
    }
    return true;
  }

 private:
  std::vector<VolSliceFit> _slices;
};

// A timestamped vol observation, for point-in-time surface builds.
struct DatedVolQuote
{
  int64_t tsNs{0};  // observation time
  double t{0.0};    // years to expiry at observation
  double k{0.0};    // log-moneyness ln(strike / forward)
  double iv{0.0};   // observed Black-Scholes implied vol
};

// Build a surface using ONLY quotes timestamped on or before asofNs — the
// no-lookahead guarantee that keeps a backtest honest. Quotes are grouped by
// expiry (t rounded to tBucket years), each group with >= 5 points is
// calibrated into a slice. Groups too thin to identify SVI are skipped.
inline VolSurface buildSurfaceAsOf(const std::vector<DatedVolQuote>& quotes, int64_t asofNs,
                                   double tBucket = 1e-4)
{
  // Bucket key by expiry so quotes for the same maturity calibrate together.
  std::vector<std::pair<double, std::vector<const DatedVolQuote*>>> groups;
  for (const auto& q : quotes)
  {
    if (q.tsNs > asofNs || q.t <= 0.0 || q.iv <= 0.0)
    {
      continue;  // future quote or degenerate — excluded
    }
    bool placed = false;
    for (auto& g : groups)
    {
      if (std::fabs(g.first - q.t) <= tBucket)
      {
        g.second.push_back(&q);
        placed = true;
        break;
      }
    }
    if (!placed)
    {
      groups.push_back({q.t, {&q}});
    }
  }

  VolSurface surface;
  for (const auto& g : groups)
  {
    if (g.second.size() < 5)
    {
      continue;
    }
    std::vector<double> ks, ws;
    ks.reserve(g.second.size());
    ws.reserve(g.second.size());
    for (const auto* q : g.second)
    {
      ks.push_back(q->k);
      ws.push_back(q->iv * q->iv * q->t);  // total variance = iv^2 * t
    }
    surface.addSlice(g.first, calibrateSVI(ks, ws));
  }
  return surface;
}

}  // namespace flox::pricing
