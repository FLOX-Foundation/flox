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
#include "flox/common.h"

#include <cmath>
#include <cstdint>

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
    return solve([&](double D)
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
    return solve([&](double y)
                 { return invariant(known, y, D); }, lo, hi, hi);
  }

  // Safeguarded Newton (Numerical Recipes rtsafe): a Newton step when it stays
  // in the bracket and makes progress, a bisection step otherwise. The bracket
  // [lo, hi] is known to straddle the root. scale sets the convergence tolerance.
  template <typename F>
  static double solve(F f, double lo, double hi, double scale)
  {
    const double tol = 1e-12 * (scale > 0.0 ? scale : 1.0);
    double flo = f(lo);
    double fhi = f(hi);
    if (flo == 0.0)
    {
      return lo;
    }
    if (fhi == 0.0)
    {
      return hi;
    }
    if ((flo > 0.0) == (fhi > 0.0))
    {
      return 0.5 * (lo + hi);  // not bracketed (degenerate); best effort
    }
    double xl = flo < 0.0 ? lo : hi;  // side where f < 0
    double xh = flo < 0.0 ? hi : lo;
    double x = 0.5 * (lo + hi);
    double dxOld = std::fabs(hi - lo);
    double dx = dxOld;
    double fx = f(x);
    double df = deriv(f, x, lo, hi);
    for (int i = 0; i < 100; ++i)
    {
      const bool newtonOutOfRange =
          ((x - xh) * df - fx) * ((x - xl) * df - fx) > 0.0;
      const bool newtonSlow = std::fabs(2.0 * fx) > std::fabs(dxOld * df);
      if (newtonOutOfRange || newtonSlow || df == 0.0)
      {
        dxOld = dx;
        dx = 0.5 * (xh - xl);
        x = xl + dx;
      }
      else
      {
        dxOld = dx;
        dx = fx / df;
        x = x - dx;
      }
      if (std::fabs(dx) < tol)
      {
        return x;
      }
      fx = f(x);
      df = deriv(f, x, lo, hi);
      if (fx < 0.0)
      {
        xl = x;
      }
      else
      {
        xh = x;
      }
    }
    return x;
  }

  // Central-difference derivative, with the step clamped inside [lo, hi].
  template <typename F>
  static double deriv(F f, double x, double lo, double hi)
  {
    double h = 1e-7 * std::fabs(x);
    if (h < 1e-12)
    {
      h = 1e-12;
    }
    double xp = x + h;
    double xm = x - h;
    if (xp > hi)
    {
      xp = hi;
    }
    if (xm < lo)
    {
      xm = lo;
    }
    const double span = xp - xm;
    if (span <= 0.0)
    {
      return 0.0;
    }
    return (f(xp) - f(xm)) / span;
  }

  Quantity _rb;
  Quantity _rq;
  double _A;
  double _gamma;
  int32_t _feeBps;
};

}  // namespace flox
