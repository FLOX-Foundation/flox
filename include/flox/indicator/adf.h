#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace flox::indicator
{

// Augmented Dickey-Fuller (ADF) stationarity test.
//
//   adf(x, max_lag, regression="c")
//
// Tests the null H0: y has a unit root (non-stationary) against the
// alternative H1: y is (trend-)stationary.
//
// Regression modes:
//   "n"  — no constant, no trend
//   "c"  — constant (drift)
//   "ct" — constant + linear trend
//
// Returns: { test_stat, p_value, used_lag }.
//   test_stat = β / SE(β) for the coefficient on y[t-1] in the regression
//      Δy[t] = α + δ*t + β*y[t-1] + Σ γ_i * Δy[t-i] + ε
//   used_lag = lag chosen via AIC over [0 .. max_lag]
//   p_value  = approximate, via MacKinnon (1996) asymptotic critical values.
//              For more precision, compare test_stat to the published critical
//              values directly.
//
// Inherently batch over a window — no streaming variant.

enum class AdfRegression : uint8_t
{
  None = 0,          // "n"
  Constant = 1,      // "c"
  ConstantTrend = 2  // "ct"
};

inline AdfRegression parseAdfRegression(const std::string& s)
{
  if (s == "n")
  {
    return AdfRegression::None;
  }
  if (s == "c")
  {
    return AdfRegression::Constant;
  }
  if (s == "ct")
  {
    return AdfRegression::ConstantTrend;
  }
  throw std::invalid_argument("adf: regression must be one of: \"n\", \"c\", \"ct\"");
}

struct AdfResult
{
  double test_stat;
  double p_value;
  size_t used_lag;
};

namespace detail
{

// OLS via normal equations: solve (X^T X) β = X^T y, return β plus the
// residual sum-of-squares and the diagonal of (X^T X)^{-1} for SE estimates.
//
// Uses Gauss-Jordan elimination on the augmented (X^T X | I | X^T y) matrix.
struct OlsFit
{
  std::vector<double> beta;     // size k
  std::vector<double> covDiag;  // diag of (X^T X)^{-1}
  double rss;                   // residual sum of squares
  size_t n;                     // sample size
  size_t k;                     // number of regressors
};

inline OlsFit olsFit(const std::vector<double>& X, size_t n, size_t k,
                     const std::vector<double>& y)
{
  // X is row-major n x k. y is size n.
  std::vector<double> XtX(k * k, 0.0);
  std::vector<double> Xty(k, 0.0);

  for (size_t i = 0; i < n; ++i)
  {
    for (size_t a = 0; a < k; ++a)
    {
      double xa = X[i * k + a];
      Xty[a] += xa * y[i];
      for (size_t b = 0; b < k; ++b)
      {
        XtX[a * k + b] += xa * X[i * k + b];
      }
    }
  }

  // Augment [XtX | I] and run Gauss-Jordan to invert.
  std::vector<double> aug(k * (2 * k), 0.0);
  for (size_t i = 0; i < k; ++i)
  {
    for (size_t j = 0; j < k; ++j)
    {
      aug[i * (2 * k) + j] = XtX[i * k + j];
    }
    aug[i * (2 * k) + k + i] = 1.0;
  }

  for (size_t col = 0; col < k; ++col)
  {
    // Partial pivoting
    size_t pivot = col;
    double best = std::abs(aug[col * (2 * k) + col]);
    for (size_t r = col + 1; r < k; ++r)
    {
      double v = std::abs(aug[r * (2 * k) + col]);
      if (v > best)
      {
        best = v;
        pivot = r;
      }
    }
    if (best < 1e-14)
    {
      throw std::runtime_error("adf: singular regression matrix");
    }
    if (pivot != col)
    {
      for (size_t j = 0; j < 2 * k; ++j)
      {
        std::swap(aug[col * (2 * k) + j], aug[pivot * (2 * k) + j]);
      }
    }

    double piv = aug[col * (2 * k) + col];
    for (size_t j = 0; j < 2 * k; ++j)
    {
      aug[col * (2 * k) + j] /= piv;
    }
    for (size_t r = 0; r < k; ++r)
    {
      if (r == col)
      {
        continue;
      }
      double factor = aug[r * (2 * k) + col];
      if (factor == 0.0)
      {
        continue;
      }
      for (size_t j = 0; j < 2 * k; ++j)
      {
        aug[r * (2 * k) + j] -= factor * aug[col * (2 * k) + j];
      }
    }
  }

  // β = (XtX)^{-1} * Xty
  std::vector<double> beta(k, 0.0);
  for (size_t i = 0; i < k; ++i)
  {
    double s = 0.0;
    for (size_t j = 0; j < k; ++j)
    {
      s += aug[i * (2 * k) + k + j] * Xty[j];
    }
    beta[i] = s;
  }

  std::vector<double> covDiag(k, 0.0);
  for (size_t i = 0; i < k; ++i)
  {
    covDiag[i] = aug[i * (2 * k) + k + i];
  }

  // RSS
  double rss = 0.0;
  for (size_t i = 0; i < n; ++i)
  {
    double pred = 0.0;
    for (size_t a = 0; a < k; ++a)
    {
      pred += X[i * k + a] * beta[a];
    }
    double r = y[i] - pred;
    rss += r * r;
  }

  return OlsFit{std::move(beta), std::move(covDiag), rss, n, k};
}

// Build the ADF regression for a given lag.
// Returns true if regression succeeded; fit is populated.
inline bool buildAdfRegression(std::span<const double> x, size_t lag, AdfRegression reg,
                               OlsFit& outFit, size_t& outBetaIdx)
{
  const size_t T = x.size();
  // We need x[t-1] and x[t-1-lag], so the first usable t is lag+1.
  // Effective sample size n = T - lag - 1.
  if (T <= lag + 1)
  {
    return false;
  }
  const size_t start = lag + 1;
  const size_t n = T - start;

  // Number of regressors:
  //   1 for y[t-1]
  // + lag terms for Δy[t-1] .. Δy[t-lag]
  // + intercept and/or trend
  size_t k = 1 + lag;
  if (reg == AdfRegression::Constant)
  {
    k += 1;
  }
  else if (reg == AdfRegression::ConstantTrend)
  {
    k += 2;
  }

  if (n <= k)
  {
    return false;
  }

  std::vector<double> X(n * k, 0.0);
  std::vector<double> y(n, 0.0);

  for (size_t i = 0; i < n; ++i)
  {
    size_t t = start + i;
    y[i] = x[t] - x[t - 1];

    size_t col = 0;
    if (reg == AdfRegression::Constant || reg == AdfRegression::ConstantTrend)
    {
      X[i * k + col++] = 1.0;
    }
    if (reg == AdfRegression::ConstantTrend)
    {
      // Trend term — start at 1 to mirror conventional formulations.
      X[i * k + col++] = static_cast<double>(i + 1);
    }
    outBetaIdx = col;
    X[i * k + col++] = x[t - 1];

    for (size_t L = 1; L <= lag; ++L)
    {
      X[i * k + col++] = x[t - L] - x[t - L - 1];
    }
  }

  outFit = olsFit(X, n, k, y);
  return true;
}

// MacKinnon (1996) asymptotic critical values for ADF test.
// Rows: regression type. Columns: probability levels in `adfPLevels`.
inline const std::vector<double>& adfPLevels()
{
  static const std::vector<double> v = {0.01, 0.025, 0.05, 0.10, 0.50, 0.90, 0.95, 0.975, 0.99};
  return v;
}

inline const std::vector<double>& adfCritValues(AdfRegression reg)
{
  // Asymptotic critical values from MacKinnon (1996).
  // Index aligned with adfPLevels().
  static const std::vector<double> none = {-2.566, -2.260, -1.941, -1.617, -0.443,
                                           0.911, 1.282, 1.645, 2.054};
  static const std::vector<double> constant = {-3.430, -3.119, -2.861, -2.567, -1.617,
                                               -0.260, 0.058, 0.330, 0.601};
  static const std::vector<double> ct = {-3.958, -3.658, -3.410, -3.121, -2.181,
                                         -0.861, -0.571, -0.314, -0.040};

  switch (reg)
  {
    case AdfRegression::None:
      return none;
    case AdfRegression::Constant:
      return constant;
    case AdfRegression::ConstantTrend:
      return ct;
  }
  return constant;
}

// Map test statistic to p-value via piecewise-linear interpolation against
// MacKinnon's asymptotic critical-value table. Monotonic in the statistic.
inline double adfPValue(double tau, AdfRegression reg)
{
  const auto& probs = adfPLevels();
  const auto& crits = adfCritValues(reg);
  assert(probs.size() == crits.size());

  if (tau <= crits.front())
  {
    return probs.front();
  }
  if (tau >= crits.back())
  {
    return probs.back();
  }

  for (size_t i = 1; i < crits.size(); ++i)
  {
    if (tau <= crits[i])
    {
      double w = (tau - crits[i - 1]) / (crits[i] - crits[i - 1]);
      return probs[i - 1] + w * (probs[i] - probs[i - 1]);
    }
  }
  return probs.back();
}

}  // namespace detail

inline AdfResult adf(std::span<const double> x, size_t max_lag,
                     AdfRegression reg = AdfRegression::Constant)
{
  if (x.size() < 4)
  {
    throw std::invalid_argument("adf: input too short (need at least 4 observations)");
  }
  for (double v : x)
  {
    if (std::isnan(v))
    {
      throw std::invalid_argument("adf: input contains NaN");
    }
  }

  // AIC-based lag selection over [0 .. max_lag]. Pick the lag minimising AIC.
  size_t bestLag = 0;
  double bestAic = std::numeric_limits<double>::infinity();
  detail::OlsFit bestFit;
  size_t bestBetaIdx = 0;
  bool found = false;

  for (size_t lag = 0; lag <= max_lag; ++lag)
  {
    detail::OlsFit fit;
    size_t betaIdx = 0;
    if (!detail::buildAdfRegression(x, lag, reg, fit, betaIdx))
    {
      break;
    }
    // AIC = n * ln(rss/n) + 2*k.
    if (fit.rss <= 0.0)
    {
      continue;
    }
    double aic = static_cast<double>(fit.n) * std::log(fit.rss / static_cast<double>(fit.n)) +
                 2.0 * static_cast<double>(fit.k);
    if (aic < bestAic)
    {
      bestAic = aic;
      bestLag = lag;
      bestFit = std::move(fit);
      bestBetaIdx = betaIdx;
      found = true;
    }
  }

  if (!found)
  {
    throw std::invalid_argument("adf: input too short for the requested max_lag and regression");
  }

  // t-statistic for the y[t-1] coefficient.
  double beta = bestFit.beta[bestBetaIdx];
  double sigma2 = bestFit.rss / static_cast<double>(bestFit.n - bestFit.k);
  if (sigma2 <= 0.0 || bestFit.covDiag[bestBetaIdx] <= 0.0)
  {
    throw std::runtime_error("adf: degenerate regression");
  }
  double se = std::sqrt(sigma2 * bestFit.covDiag[bestBetaIdx]);
  double tau = beta / se;

  return AdfResult{tau, detail::adfPValue(tau, reg), bestLag};
}

inline AdfResult adf(std::span<const double> x, size_t max_lag, const std::string& regression)
{
  return adf(x, max_lag, parseAdfRegression(regression));
}

}  // namespace flox::indicator
