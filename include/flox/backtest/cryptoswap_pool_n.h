/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/detail/safeguarded_solve.h"
#include "flox/backtest/ntoken_curve.h"
#include "flox/common.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace flox
{

// Curve V2 cryptoswap with n coins, as an INTokenCurve, for a basket of volatile
// assets (a tricrypto pool). This is the static invariant: the price_scale
// vector is fixed (W19-T005 adds the repegging that moves it). The pool works in
// transformed balances xp, where coin 0 is the numeraire and every other coin is
// scaled into it: xp[0] = bal[0], xp[k] = bal[k] * price_scale[k-1]. In xp-space
// the invariant is the same superposition of constant-product and stableswap as
// the two-coin curve, with
//
//   K0 = Prod(xp) * N^N / D^N,   K = A * K0 * gamma^2 / (gamma + 1 - K0)^2,
//   F  = K * D^(N-1) * S + Prod(xp) - K * D^N - (D/N)^N = 0,  S = sum(xp).
//
// The invariant is non-monotonic, so D and the swap output both come from a
// safeguarded solve on the physical branch K0 <= 1, which is bracketed and
// pole-free. price_scale has tokenCount-1 entries (the price of each coin k>=1
// in coin 0).
class CryptoswapPoolN : public INTokenCurve
{
 public:
  CryptoswapPoolN(std::vector<double> balances, std::vector<double> priceScale, double A,
                  double gamma, int32_t feeBps)
      : _b(std::move(balances)),
        _scale(std::move(priceScale)),
        _A(A),
        _gamma(gamma),
        _feeBps(feeBps)
  {
  }

  const std::vector<double>& balances() const { return _b; }
  const std::vector<double>& priceScale() const { return _scale; }
  double amp() const { return _A; }
  double gamma() const { return _gamma; }
  int32_t feeBps() const { return _feeBps; }

  std::size_t tokenCount() const override { return _b.size(); }

  // Marginal token-i per token-j, from a tiny no-fee probe of token j in.
  Price spotPrice(std::size_t i, std::size_t j) const override
  {
    if (_b[i] <= 0.0 || _b[j] <= 0.0)
    {
      return Price{};
    }
    double sum = 0.0;
    for (double v : _b)
    {
      sum += v;
    }
    const double dx = sum * 1e-8;
    const double outI = rawOut(j, i, dx);  // token j in, token i out
    return Price::fromDouble(outI / dx);
  }

  Quantity amountOut(std::size_t i, std::size_t j, Quantity amountIn) const override
  {
    const double in = amountIn.toDouble();
    if (in <= 0.0 || _b[i] <= 0.0 || _b[j] <= 0.0)
    {
      return Quantity{};
    }
    const double inWithFee = in * feeFraction();
    const double out = rawOut(i, j, inWithFee);
    return Quantity::fromDouble(out > 0.0 ? out : 0.0);
  }

  double priceImpact(std::size_t i, std::size_t j, Quantity amountIn) const override
  {
    const double in = amountIn.toDouble();
    const double out = amountOut(i, j, amountIn).toDouble();
    if (in <= 0.0 || out <= 0.0)
    {
      return 0.0;
    }
    const double spotRate = spotPrice(j, i).toDouble();  // token-j out per token-i in, no fee
    if (spotRate <= 0.0)
    {
      return 0.0;
    }
    return 1.0 - (out / in) / spotRate;
  }

  Quantity applySwap(std::size_t i, std::size_t j, Quantity amountIn) override
  {
    const Quantity out = amountOut(i, j, amountIn);
    _b[i] += amountIn.toDouble();
    _b[j] -= out.toDouble();
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<CryptoswapPoolN>(*this);
  }

 protected:
  // Fraction of an input that survives the fee. Constant here; the repegging
  // pool overrides it with a fee that depends on how balanced the pool is.
  virtual double feeFraction() const { return 1.0 - static_cast<double>(_feeBps) / 10000.0; }

  double scaleOf(std::size_t k) const { return k == 0 ? 1.0 : _scale[k - 1]; }

  std::vector<double> xpOf() const
  {
    std::vector<double> xp(_b.size());
    for (std::size_t k = 0; k < _b.size(); ++k)
    {
      xp[k] = _b[k] * scaleOf(k);
    }
    return xp;
  }

  double kOf(double K0) const
  {
    const double denom = _gamma + 1.0 - K0;
    return _A * K0 * _gamma * _gamma / (denom * denom);
  }

  // Invariant value for a full xp vector and candidate D.
  double invariant(const std::vector<double>& xp, double D) const
  {
    const double n = static_cast<double>(xp.size());
    double S = 0.0, prod = 1.0;
    for (double v : xp)
    {
      S += v;
      prod *= v;
    }
    double nn = 1.0;
    for (std::size_t k = 0; k < xp.size(); ++k)
    {
      nn *= n;
    }
    const double K0 = prod * nn / std::pow(D, n);
    const double K = kOf(K0);
    const double dOverN = D / n;
    double dOverNpow = 1.0;
    for (std::size_t k = 0; k < xp.size(); ++k)
    {
      dOverNpow *= dOverN;
    }
    return K * std::pow(D, n - 1.0) * S + prod - K * std::pow(D, n) - dOverNpow;
  }

  // Invariant D for the current xp, by safeguarded solve on [geomean*N-ish].
  // The physical branch has K0 <= 1, i.e. D >= N*geomean(xp); the upper end is
  // the constant-sum bound sum(xp). F >= 0 at the low end, F <= 0 at the high
  // end, pole-free between.
  double getD(const std::vector<double>& xp) const
  {
    const double n = static_cast<double>(xp.size());
    double S = 0.0, logsum = 0.0;
    bool zero = false;
    for (double v : xp)
    {
      S += v;
      if (v <= 0.0)
      {
        zero = true;
      }
      else
      {
        logsum += std::log(v);
      }
    }
    if (zero || S <= 0.0)
    {
      return 0.0;
    }
    const double geomean = std::exp(logsum / n);
    const double lo = n * geomean;  // K0 = 1 here
    const double hi = S;            // constant-sum bound
    if (hi <= lo)
    {
      return lo;
    }
    return detail::safeguardedRoot([&](double D)
                                   { return invariant(xp, D); }, lo, hi, hi);
  }

  // Balance xp[j] (the output coin) that holds the invariant for the other xp
  // values and D. The cryptoswap invariant is non-monotonic and has more than
  // one root in xp[j]; the physical one is the topmost -- the first crossing
  // below the coin's current balance, since removing the output coin walks
  // xp[j] down from there. A search from a midpoint would latch onto the lower
  // spurious root, especially near balance. So descend gradually from the
  // current value to bracket the first sign change, then rtsafe inside that
  // tight bracket, which contains only the physical root.
  double getXp(std::vector<double> xp, std::size_t j, double D) const
  {
    auto f = [&](double yy)
    {
      xp[j] = yy;
      return invariant(xp, D);
    };
    const double hi = xp[j] > 0.0 ? xp[j] : D / static_cast<double>(xp.size());
    const double fhi = f(hi);
    if (fhi == 0.0)
    {
      return hi;
    }
    // Output coin: the pool holds extra value before the output is removed, so
    // f(current) > 0 and the root is below. Descend to the first f <= 0.
    double lo = hi;
    double flo = fhi;
    for (int s = 0; s < 2000 && flo > 0.0; ++s)
    {
      lo *= 0.99;
      flo = f(lo);
      if (lo < hi * 1e-15)
      {
        break;
      }
    }
    if ((flo > 0.0) == (fhi > 0.0))
    {
      return lo;  // no crossing found (degenerate); best effort
    }
    return detail::safeguardedRoot(f, lo, hi, hi);
  }

  // No-fee output of token j for amountIn of token i, in real units.
  double rawOut(std::size_t i, std::size_t j, double inReal) const
  {
    std::vector<double> xp = xpOf();
    const double D = getD(xp);
    const double oldXpJ = xp[j];
    xp[i] += inReal * scaleOf(i);
    const double newXpJ = getXp(xp, j, D);
    const double outXp = oldXpJ - newXpJ;
    return outXp / scaleOf(j);
  }

  std::vector<double> _b;
  std::vector<double> _scale;
  double _A;
  double _gamma;
  int32_t _feeBps;
};

}  // namespace flox
