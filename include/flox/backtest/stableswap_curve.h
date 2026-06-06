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

// Curve StableSwap curve (two-coin), for assets that trade near a peg. The
// invariant blends constant-sum (flat price near the peg) and constant-product
// (so the pool never runs dry): A*n^n*S + D = A*D*n^n + D^(n+1)/(n^n*Prod),
// where S is the sum of balances, Prod their product, and A the amplification
// coefficient. A high A keeps the price close to 1 across a wide range and only
// curves sharply once the pool is far from balance.
//
// Neither D nor the output is closed-form: both come from Newton iteration. D
// is the invariant value for the current balances; an output balance solves a
// quadratic against the new input balance. The math runs in double, enough for
// a backtest. base is coin 0, quote is coin 1, price = quote per base.
class StableSwapCurve : public IAmmCurve
{
 public:
  StableSwapCurve(Quantity balanceBase, Quantity balanceQuote, double A, int32_t feeBps)
      : _rb(balanceBase), _rq(balanceQuote), _A(A), _feeBps(feeBps)
  {
  }

  Quantity reserveBase() const { return _rb; }
  Quantity reserveQuote() const { return _rq; }
  double amp() const { return _A; }
  int32_t feeBps() const { return _feeBps; }

  void setReserves(Quantity balanceBase, Quantity balanceQuote)
  {
    _rb = balanceBase;
    _rq = balanceQuote;
  }

  // Marginal quote per base, read off a tiny no-fee probe swap.
  Price spotPrice() const override
  {
    const double x = _rb.toDouble();
    const double y = _rq.toDouble();
    if (x <= 0.0 || y <= 0.0)
    {
      return Price{};
    }
    const double dx = (x + y) * 1e-8;
    const double yNew = getY(x + dx, getD(x, y));
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
      out = y - getY(x + inWithFee, D);  // base in, quote out
    }
    else
    {
      out = x - getY(y + inWithFee, D);  // quote in, base out
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
  // Invariant D for the two balances, by Newton. n = 2 so Ann = A*4.
  double getD(double x, double y) const
  {
    const double S = x + y;
    if (S <= 0.0)
    {
      return 0.0;
    }
    const double Ann = _A * 4.0;
    double D = S;
    for (int i = 0; i < 255; ++i)
    {
      double dP = D;
      dP = dP * D / (x * 2.0);
      dP = dP * D / (y * 2.0);
      const double Dprev = D;
      D = (Ann * S + dP * 2.0) * D / ((Ann - 1.0) * D + 3.0 * dP);
      if (std::fabs(D - Dprev) <= 1e-10)
      {
        break;
      }
    }
    return D;
  }

  // Output-coin balance that holds the invariant for the given input balance,
  // by Newton on y^2 + (b - D)*y - c = 0.
  double getY(double xNew, double D) const
  {
    const double Ann = _A * 4.0;
    double c = D;
    c = c * D / (xNew * 2.0);
    c = c * D / (Ann * 2.0);
    const double b = xNew + D / Ann;
    double y = D;
    for (int i = 0; i < 255; ++i)
    {
      const double yPrev = y;
      y = (y * y + c) / (2.0 * y + b - D);
      if (std::fabs(y - yPrev) <= 1e-10)
      {
        break;
      }
    }
    return y;
  }

  Quantity _rb;
  Quantity _rq;
  double _A;
  int32_t _feeBps;
};

}  // namespace flox
