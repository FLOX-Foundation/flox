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
#include "flox/util/int/u256.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace flox
{

// Curve V2 cryptoswap pool (tricrypto2 style), exact in native-wei integer math,
// reproducing get_dy to the wei. This is a direct transcription of the contract's
// integer algorithm (CurveCryptoMath3 newton_D / newton_y / reduction_coefficient
// and CurveCryptoViews3 get_dy): the divisions floor exactly where the Vyper
// floors, so the rounding matches, not just the formula. ANN is A * N^N (the
// value the contract's A() returns, already times A_MULTIPLIER = 10000); gamma
// is in 1e18. The balances are normalized by per-coin PRECISIONS and the price
// scale before the solve.
//
// This is the pricing surface. The price scale is held across applySwap here;
// the internal repegging that moves it (tweak_price) is a separate concern.
class CryptoswapCurve : public INTokenCurve
{
 public:
  // balances / precisions are per coin; priceScale has n-1 entries (the price of
  // each coin k>=1 in coin 0, 1e18). A is the contract A() value, gamma the
  // gamma() value, and midFee / outFee / feeGamma the dynamic-fee parameters.
  CryptoswapCurve(std::vector<u256> balances, std::vector<u256> precisions,
                  std::vector<u256> priceScale, uint64_t A, u256 gamma, u256 midFee, u256 outFee,
                  u256 feeGamma)
      : _b(std::move(balances)),
        _prec(std::move(precisions)),
        _scale(std::move(priceScale)),
        _ANN(A),
        _gamma(gamma),
        _midFee(midFee),
        _outFee(outFee),
        _feeGamma(feeGamma)
  {
  }

  uint64_t amp() const { return _ANN; }
  const std::vector<u256>& priceScale() const { return _scale; }

  std::size_t tokenCount() const override { return _b.size(); }
  const std::vector<u256>& balances() const override { return _b; }

  u256 amountOut(std::size_t i, std::size_t j, const u256& dx) const override
  {
    if (dx.isZero())
    {
      return u256(0);
    }
    const u256 D = newtonD(xpOf(_b));
    std::vector<u256> bal = _b;
    bal[i] = bal[i] + dx;
    std::vector<u256> xp = xpOf(bal);
    const u256 y = newtonY(xp, D, j);
    if (xp[j] <= y + u256(1))
    {
      return u256(0);
    }
    u256 dy = xp[j] - y - u256(1);
    xp[j] = y;
    if (j > 0)
    {
      dy = dy * P() / _scale[j - 1];
    }
    dy = dy / _prec[j];
    const u256 rc = reductionCoefficient(xp);
    const u256 f = (_midFee * rc + _outFee * (P() - rc)) / P();
    return dy - f * dy / feeDenom();
  }

  u256 applySwap(std::size_t i, std::size_t j, const u256& dx) override
  {
    const u256 out = amountOut(i, j, dx);
    _b[i] = _b[i] + dx;
    _b[j] = _b[j] - out;
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<CryptoswapCurve>(*this);
  }

 protected:
  static u256 P() { return u256::pow10(18); }
  static u256 feeDenom() { return u256::pow10(10); }
  static constexpr uint64_t A_MULT = 10000;

  std::size_t N() const { return _b.size(); }

  // Normalized balances: coin 0 by its precision, the rest by precision and price
  // scale, exactly as the contract's xp transform.
  std::vector<u256> xpOf(const std::vector<u256>& bal) const
  {
    std::vector<u256> xp = bal;
    xp[0] = xp[0] * _prec[0];
    for (std::size_t k = 0; k + 1 < N(); ++k)
    {
      xp[k + 1] = xp[k + 1] * _scale[k] * _prec[k + 1] / P();
    }
    return xp;
  }

  static std::vector<u256> sortDesc(std::vector<u256> a)
  {
    for (std::size_t i = 1; i < a.size(); ++i)
    {
      const u256 x = a[i];
      std::size_t cur = i;
      for (std::size_t s = 0; s < a.size(); ++s)
      {
        const u256 y = a[cur - 1];
        if (y > x)
        {
          break;
        }
        a[cur] = y;
        --cur;
        if (cur == 0)
        {
          break;
        }
      }
      a[cur] = x;
    }
    return a;
  }

  static u256 maxU(const u256& a, const u256& b) { return a > b ? a : b; }
  static u256 absDiff(const u256& a, const u256& b) { return a > b ? a - b : b - a; }

  u256 geometricMean(const std::vector<u256>& ux, bool doSort) const
  {
    const std::vector<u256> x = doSort ? sortDesc(ux) : ux;
    const u256 n(N());
    u256 D = x[0];
    for (int it = 0; it < 255; ++it)
    {
      const u256 Dprev = D;
      u256 tmp = P();
      for (const u256& xi : x)
      {
        tmp = tmp * xi / D;
      }
      D = D * (u256(N() - 1) * P() + tmp) / (n * P());
      const u256 diff = absDiff(D, Dprev);
      if (diff <= u256(1) || diff * P() < D)
      {
        return D;
      }
    }
    return D;
  }

  u256 newtonD(const std::vector<u256>& xUnsorted) const
  {
    const std::vector<u256> x = sortDesc(xUnsorted);
    const u256 n(N());
    u256 D = n * geometricMean(x, false);
    u256 S(0);
    for (const u256& xi : x)
    {
      S = S + xi;
    }
    for (int it = 0; it < 255; ++it)
    {
      const u256 Dprev = D;
      u256 K0 = P();
      for (const u256& xi : x)
      {
        K0 = K0 * xi * n / D;
      }
      u256 g1k0 = _gamma + P();
      g1k0 = (g1k0 > K0) ? (g1k0 - K0 + u256(1)) : (K0 - g1k0 + u256(1));
      u256 mul1 = P() * D / _gamma * g1k0 / _gamma * g1k0 * u256(A_MULT) / u256(_ANN);
      u256 mul2 = (u256(2) * P()) * n * K0 / g1k0;
      u256 negFprime = (S + S * mul2 / P()) + mul1 * n / K0 - mul2 * D / P();
      u256 Dplus = D * (negFprime + S) / negFprime;
      u256 Dminus = D * D / negFprime;
      if (P() > K0)
      {
        Dminus = Dminus + D * (mul1 / negFprime) / P() * (P() - K0) / K0;
      }
      else
      {
        Dminus = Dminus - D * (mul1 / negFprime) / P() * (K0 - P()) / K0;
      }
      D = (Dplus > Dminus) ? (Dplus - Dminus) : (Dminus - Dplus) / u256(2);
      if (absDiff(D, Dprev) * u256::pow10(14) < maxU(u256::pow10(16), D))
      {
        return D;
      }
    }
    return D;
  }

  u256 newtonY(const std::vector<u256>& x, const u256& D, std::size_t i) const
  {
    const u256 n(N());
    u256 y = D / n;
    u256 K0i = P();
    u256 Si(0);
    std::vector<u256> xs = x;
    xs[i] = u256(0);
    xs = sortDesc(xs);
    u256 conv = maxU(maxU(xs[0] / u256::pow10(14), D / u256::pow10(14)), u256(100));
    for (std::size_t jj = 2; jj <= N(); ++jj)
    {
      const u256& _x = xs[N() - jj];
      y = y * D / (_x * n);
      Si = Si + _x;
    }
    for (std::size_t jj = 0; jj + 1 < N(); ++jj)
    {
      K0i = K0i * xs[jj] * n / D;
    }
    for (int it = 0; it < 255; ++it)
    {
      const u256 yprev = y;
      u256 K0 = K0i * y * n / D;
      u256 Ssum = Si + y;
      u256 g1k0 = _gamma + P();
      g1k0 = (g1k0 > K0) ? (g1k0 - K0 + u256(1)) : (K0 - g1k0 + u256(1));
      u256 mul1 = P() * D / _gamma * g1k0 / _gamma * g1k0 * u256(A_MULT) / u256(_ANN);
      u256 mul2 = P() + (u256(2) * P()) * K0 / g1k0;
      u256 yfprime = P() * y + Ssum * mul2 + mul1;
      const u256 dyfprime = D * mul2;
      if (yfprime < dyfprime)
      {
        y = yprev / u256(2);
        continue;
      }
      yfprime = yfprime - dyfprime;
      const u256 fprime = yfprime / y;
      u256 yMinus = mul1 / fprime;
      u256 yPlus = (yfprime + P() * D) / fprime + yMinus * P() / K0;
      yMinus = yMinus + P() * Ssum / fprime;
      y = (yPlus < yMinus) ? (yprev / u256(2)) : (yPlus - yMinus);
      if (absDiff(y, yprev) < maxU(conv, y / u256::pow10(14)))
      {
        return y;
      }
    }
    return y;
  }

  // fee_gamma / (fee_gamma + (1 - K)), K = prod(x) / (sum(x)/N)^N, all 1e18.
  u256 reductionCoefficient(const std::vector<u256>& x) const
  {
    const u256 n(N());
    u256 K = P();
    u256 S(0);
    for (const u256& xi : x)
    {
      S = S + xi;
    }
    for (const u256& xi : x)
    {
      K = K * n * xi / S;
    }
    if (!_feeGamma.isZero())
    {
      K = _feeGamma * P() / (_feeGamma + P() - K);
    }
    return K;
  }

  std::vector<u256> _b;
  std::vector<u256> _prec;
  std::vector<u256> _scale;
  uint64_t _ANN;
  u256 _gamma;
  u256 _midFee;
  u256 _outFee;
  u256 _feeGamma;
};

}  // namespace flox
