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

namespace flox
{

// Balancer-style weighted-pool curve (two-token). The invariant is
// B_base^w_base * B_quote^w_quote = const, with the weights summing to 1; equal
// weights (0.5, 0.5) reduce to constant-product. A higher weight on a token
// raises its spot price for the same balance. Closed-form: amountOut needs only
// the balances, the weights, and the fee, plus a pow().
class WeightedCurve : public IAmmCurve
{
 public:
  WeightedCurve(Quantity reserveBase, Quantity reserveQuote, double weightBase, double weightQuote,
                int32_t feeBps)
      : _rb(reserveBase),
        _rq(reserveQuote),
        _wBase(weightBase),
        _wQuote(weightQuote),
        _feeBps(feeBps)
  {
  }

  Quantity reserveBase() const { return _rb; }
  Quantity reserveQuote() const { return _rq; }
  double weightBase() const { return _wBase; }
  double weightQuote() const { return _wQuote; }
  int32_t feeBps() const { return _feeBps; }

  void setReserves(Quantity reserveBase, Quantity reserveQuote)
  {
    _rb = reserveBase;
    _rq = reserveQuote;
  }

  // Quote per base: (B_quote/w_quote) / (B_base/w_base).
  Price spotPrice() const override
  {
    const double b = _rb.toDouble();
    if (b <= 0.0 || _wBase <= 0.0 || _wQuote <= 0.0)
    {
      return Price{};
    }
    return Price::fromDouble((_rq.toDouble() / _wQuote) / (b / _wBase));
  }

  Quantity amountOut(Quantity amountIn, bool baseForQuote) const override
  {
    const double bIn = baseForQuote ? _rb.toDouble() : _rq.toDouble();
    const double bOut = baseForQuote ? _rq.toDouble() : _rb.toDouble();
    const double wIn = baseForQuote ? _wBase : _wQuote;
    const double wOut = baseForQuote ? _wQuote : _wBase;
    const double in = amountIn.toDouble();
    if (in <= 0.0 || bIn <= 0.0 || bOut <= 0.0 || wIn <= 0.0 || wOut <= 0.0)
    {
      return Quantity{};
    }
    const double inWithFee = in * (1.0 - static_cast<double>(_feeBps) / 10000.0);
    const double out = bOut * (1.0 - std::pow(bIn / (bIn + inWithFee), wIn / wOut));
    return Quantity::fromDouble(out);
  }

  double priceImpact(Quantity amountIn, bool baseForQuote) const override
  {
    const double bIn = baseForQuote ? _rb.toDouble() : _rq.toDouble();
    const double bOut = baseForQuote ? _rq.toDouble() : _rb.toDouble();
    const double wIn = baseForQuote ? _wBase : _wQuote;
    const double wOut = baseForQuote ? _wQuote : _wBase;
    const double in = amountIn.toDouble();
    const double out = amountOut(amountIn, baseForQuote).toDouble();
    if (in <= 0.0 || bIn <= 0.0 || out <= 0.0 || wIn <= 0.0 || wOut <= 0.0)
    {
      return 0.0;
    }
    const double spotRate = (bOut / wOut) / (bIn / wIn);  // marginal out per in, no fee
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
  Quantity _rb;
  Quantity _rq;
  double _wBase;
  double _wQuote;
  int32_t _feeBps;
};

}  // namespace flox
