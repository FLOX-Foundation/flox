/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/ntoken_curve.h"
#include "flox/common.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace flox
{

// Curve StableSwap with n coins, as an INTokenCurve, for a basket of pegged
// assets (a 3pool of stablecoins). Same blend of constant-sum and
// constant-product as the two-coin StableSwapCurve, generalized to n coins: the
// invariant D and the swap output both come from Newton iteration over all n
// balances. The invariant is
//
//   Ann*S + D = Ann*D + D^(n+1)/(n^n * Prod(x)),   Ann = A * n^n,
//
// with S the sum of balances. A swap between i and j solves get_y for coin j
// against the bumped balance of coin i, holding the others fixed.
class StableSwapPoolN : public INTokenCurve
{
 public:
  StableSwapPoolN(std::vector<double> balances, double A, int32_t feeBps)
      : _b(std::move(balances)), _A(A), _feeBps(feeBps)
  {
  }

  const std::vector<double>& balances() const { return _b; }
  double amp() const { return _A; }
  int32_t feeBps() const { return _feeBps; }

  std::size_t tokenCount() const override { return _b.size(); }

  // Marginal token-i per token-j, from a tiny no-fee probe of token j in.
  Price spotPrice(std::size_t i, std::size_t j) const override
  {
    if (_b[i] <= 0.0 || _b[j] <= 0.0)
    {
      return Price{};
    }
    const double D = getD(_b);
    double sum = 0.0;
    for (double v : _b)
    {
      sum += v;
    }
    const double dx = sum * 1e-9;
    std::vector<double> x = _b;
    x[j] += dx;
    const double outI = _b[i] - getY(x, i, D);
    return Price::fromDouble(outI / dx);
  }

  Quantity amountOut(std::size_t i, std::size_t j, Quantity amountIn) const override
  {
    const double in = amountIn.toDouble();
    if (in <= 0.0 || _b[i] <= 0.0 || _b[j] <= 0.0)
    {
      return Quantity{};
    }
    const double inWithFee = in * (1.0 - static_cast<double>(_feeBps) / 10000.0);
    const double D = getD(_b);
    std::vector<double> x = _b;
    x[i] += inWithFee;
    const double out = _b[j] - getY(x, j, D);
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
    return std::make_unique<StableSwapPoolN>(*this);
  }

 private:
  double annOf(std::size_t n) const
  {
    double npow = 1.0;
    for (std::size_t k = 0; k < n; ++k)
    {
      npow *= static_cast<double>(n);
    }
    return _A * npow;
  }

  // Invariant D over all balances, by Newton.
  double getD(const std::vector<double>& x) const
  {
    const std::size_t n = x.size();
    double S = 0.0;
    for (double v : x)
    {
      S += v;
    }
    if (S <= 0.0)
    {
      return 0.0;
    }
    const double Ann = annOf(n);
    const double dn = static_cast<double>(n);
    double D = S;
    for (int iter = 0; iter < 255; ++iter)
    {
      double dP = D;
      for (double v : x)
      {
        dP = dP * D / (v * dn);
      }
      const double Dprev = D;
      D = (Ann * S + dP * dn) * D / ((Ann - 1.0) * D + (dn + 1.0) * dP);
      if (std::fabs(D - Dprev) <= 1e-10 * (D > 1.0 ? D : 1.0))
      {
        break;
      }
    }
    return D;
  }

  // Balance of coin j that holds the invariant for the other balances and D, by
  // Newton on y = (y^2 + c)/(2y + b - D).
  double getY(const std::vector<double>& x, std::size_t j, double D) const
  {
    const std::size_t n = x.size();
    const double Ann = annOf(n);
    const double dn = static_cast<double>(n);
    double c = D;
    double sOthers = 0.0;
    for (std::size_t k = 0; k < n; ++k)
    {
      if (k == j)
      {
        continue;
      }
      sOthers += x[k];
      c = c * D / (x[k] * dn);
    }
    c = c * D / (Ann * dn);
    const double b = sOthers + D / Ann;
    double y = D;
    for (int iter = 0; iter < 255; ++iter)
    {
      const double yprev = y;
      y = (y * y + c) / (2.0 * y + b - D);
      if (std::fabs(y - yprev) <= 1e-10 * (y > 1.0 ? y : 1.0))
      {
        break;
      }
    }
    return y;
  }

  std::vector<double> _b;
  double _A;
  int32_t _feeBps;
};

}  // namespace flox
