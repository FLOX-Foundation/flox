/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/amm_curve.h"
#include "flox/backtest/detail/safeguarded_solve.h"
#include "flox/common.h"

#include <cmath>
#include <cstdint>
#include <memory>

namespace flox
{

// Curve V2 cryptoswap curve (two-coin), for volatile pairs. The invariant is a
// superposition of constant-product and stableswap: a coefficient K that is
// large near balance (so the curve is flat and stableswap-like there) and
// decays toward zero as the pool drifts away from balance (so it falls back to
// constant-product and never empties). With
//
//   K0 = N^N * Prod(x) / D^N,   K = A * gamma^2 * K0 / (gamma + 1 - K0)^2,
//
// the invariant is K*D^(N-1)*S + Prod(x) = K*D^N + (D/N)^N, where S is the sum
// of balances. A sets the depth near balance, gamma how fast K decays away from
// it.
//
// The invariant is non-monotonic, so neither D nor the swap output is solved by
// plain Newton. Both come from a safeguarded Newton (Newton step with a
// bisection fallback) over the physical branch K0 <= 1, which is bracketed and
// pole-free. The math runs in double, enough for a backtest. base is coin 0,
// quote is coin 1, price = quote per base.
class CryptoswapCurve : public IAmmCurve
{
 public:
  CryptoswapCurve(Quantity balanceBase, Quantity balanceQuote, double A, double gamma,
                  int32_t feeBps)
      : _rb(balanceBase), _rq(balanceQuote), _A(A), _gamma(gamma), _feeBps(feeBps)
  {
  }

  Quantity reserveBase() const { return _rb; }
  Quantity reserveQuote() const { return _rq; }
  double amp() const { return _A; }
  double gamma() const { return _gamma; }
  int32_t feeBps() const { return _feeBps; }

  void setReserves(Quantity balanceBase, Quantity balanceQuote)
  {
    _rb = balanceBase;
    _rq = balanceQuote;
  }

  Price spotPrice() const override
  {
    const double x = _rb.toDouble();
    const double y = _rq.toDouble();
    if (x <= 0.0 || y <= 0.0)
    {
      return Price{};
    }
    const double D = getD(x, y);
    const double dx = (x + y) * 1e-8;
    const double yNew = getOther(x + dx, D);
    return Price::fromDouble((y - yNew) / dx);
  }

  Quantity amountOut(Quantity amountIn, bool baseForQuote) const override
  {
    const double in = amountIn.toDouble();
    const double x = _rb.toDouble();
    const double y = _rq.toDouble();
    if (in <= 0.0 || x <= 0.0 || y <= 0.0)
    {
      return Quantity{};
    }
    const double inWithFee = in * (1.0 - static_cast<double>(_feeBps) / 10000.0);
    const double D = getD(x, y);
    double out;
    if (baseForQuote)
    {
      out = y - getOther(x + inWithFee, D);  // base in, quote out
    }
    else
    {
      out = x - getOther(y + inWithFee, D);  // quote in, base out
    }
    return Quantity::fromDouble(out > 0.0 ? out : 0.0);
  }

  double priceImpact(Quantity amountIn, bool baseForQuote) const override
  {
    const double in = amountIn.toDouble();
    const double out = amountOut(amountIn, baseForQuote).toDouble();
    if (in <= 0.0 || out <= 0.0)
    {
      return 0.0;
    }
    const double spot = spotPrice().toDouble();
    const double spotRate = baseForQuote ? spot : 1.0 / spot;  // marginal out per in, no fee
    if (spotRate <= 0.0)
    {
      return 0.0;
    }
    const double execRate = out / in;
    return 1.0 - execRate / spotRate;
  }

  Quantity applySwap(Quantity amountIn, bool baseForQuote) override
  {
    const Quantity out = amountOut(amountIn, baseForQuote);
    if (baseForQuote)
    {
      _rb = _rb + amountIn;
      _rq = Quantity::fromRaw(_rq.raw() - out.raw());
    }
    else
    {
      _rq = _rq + amountIn;
      _rb = Quantity::fromRaw(_rb.raw() - out.raw());
    }
    return out;
  }

  std::unique_ptr<IAmmCurve> clone() const override
  {
    return std::make_unique<CryptoswapCurve>(*this);
  }

 private:
  // K0 = N^N * Prod / D^N; for N = 2 that is 4*x*y/D^2.
  static double k0Of(double x, double y, double D) { return 4.0 * x * y / (D * D); }

  double kOf(double K0) const
  {
    const double denom = _gamma + 1.0 - K0;
    return _A * _gamma * _gamma * K0 / (denom * denom);
  }

  // Invariant value with one balance fixed; for N = 2,
  // F = K*D*(x+y) + x*y - K*D^2 - D^2/4. Zero on the curve.
  double invariant(double x, double y, double D) const
  {
    const double K = kOf(k0Of(x, y, D));
    return K * D * (x + y) + x * y - K * D * D - 0.25 * D * D;
  }

  // Invariant D for the current balances. On the physical branch D lies in
  // [2*sqrt(x*y), x+y]: F >= 0 at the low end, F <= 0 at the high end, and K0
  // stays <= 1 so the pole at K0 = gamma+1 is never approached.
  double getD(double x, double y) const
  {
    if (x <= 0.0 || y <= 0.0)
    {
      return 0.0;
    }
    const double lo = 2.0 * std::sqrt(x * y);
    const double hi = x + y;
    return detail::safeguardedRoot([&](double D)
                                   { return invariant(x, y, D); }, lo, hi, hi);
  }

  // Partner balance that holds the invariant for the given known balance and D.
  // The physical solution keeps the product below D^2/4 (K0 <= 1), so the other
  // balance is bracketed by (0, D^2/(4*known)]: F < 0 as it goes to zero, F >= 0
  // at the balanced point.
  double getOther(double known, double D) const
  {
    if (known <= 0.0 || D <= 0.0)
    {
      return 0.0;
    }
    const double hi = D * D / (4.0 * known);
    const double lo = hi * 1e-12;
    return detail::safeguardedRoot([&](double y)
                                   { return invariant(known, y, D); }, lo, hi, hi);
  }

  Quantity _rb;
  Quantity _rq;
  double _A;
  double _gamma;
  int32_t _feeBps;
};

}  // namespace flox
