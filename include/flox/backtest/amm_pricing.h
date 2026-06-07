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

#include <memory>

namespace flox
{

// Constant-product (Uniswap v2 style) AMM curve. A swap fills at the price the
// reserves imply, not against an order book, so the output amount and the
// price impact come from the x * y = k curve and the fee tier. The simplest
// IAmmCurve: it needs only the two reserves and the fee.
class ConstantProductCurve : public IAmmCurve
{
 public:
  // reserveBase / reserveQuote are the two reserves; feeBps is the swap fee in
  // basis points (30 = 0.30%, the common v2 tier).
  ConstantProductCurve(Quantity reserveBase, Quantity reserveQuote, int32_t feeBps)
      : _reserveBase(reserveBase), _reserveQuote(reserveQuote), _feeBps(feeBps)
  {
  }

  Quantity reserveBase() const { return _reserveBase; }
  Quantity reserveQuote() const { return _reserveQuote; }
  int32_t feeBps() const { return _feeBps; }

  void setReserves(Quantity reserveBase, Quantity reserveQuote)
  {
    _reserveBase = reserveBase;
    _reserveQuote = reserveQuote;
  }

  Price spotPrice() const override
  {
    const double base = _reserveBase.toDouble();
    if (base <= 0.0)
    {
      return Price{};
    }
    return Price::fromDouble(_reserveQuote.toDouble() / base);
  }

  // baseForQuote=true swaps base->quote (a sell of base), false swaps
  // quote->base (a buy of base). Applies the fee and the constant-product
  // curve. Reserves are not mutated.
  Quantity amountOut(Quantity amountIn, bool baseForQuote) const override
  {
    const double reserveIn = baseForQuote ? _reserveBase.toDouble() : _reserveQuote.toDouble();
    const double reserveOut = baseForQuote ? _reserveQuote.toDouble() : _reserveBase.toDouble();
    const double in = amountIn.toDouble();
    if (in <= 0.0 || reserveIn <= 0.0 || reserveOut <= 0.0)
    {
      return Quantity{};
    }
    const double inWithFee = in * (1.0 - static_cast<double>(_feeBps) / 10000.0);
    const double out = reserveOut * inWithFee / (reserveIn + inWithFee);
    return Quantity::fromDouble(out);
  }

  double priceImpact(Quantity amountIn, bool baseForQuote) const override
  {
    const double reserveIn = baseForQuote ? _reserveBase.toDouble() : _reserveQuote.toDouble();
    const double reserveOut = baseForQuote ? _reserveQuote.toDouble() : _reserveBase.toDouble();
    const double in = amountIn.toDouble();
    const double out = amountOut(amountIn, baseForQuote).toDouble();
    if (in <= 0.0 || reserveIn <= 0.0 || out <= 0.0)
    {
      return 0.0;
    }
    const double spotRate = reserveOut / reserveIn;  // out per in, no fee
    const double execRate = out / in;
    return 1.0 - execRate / spotRate;
  }

  // Execute the swap: return the output and move the reserves along the curve,
  // so a sequence of swaps in a backtest sees the pool drift.
  Quantity applySwap(Quantity amountIn, bool baseForQuote) override
  {
    const Quantity out = amountOut(amountIn, baseForQuote);
    if (baseForQuote)
    {
      _reserveBase = _reserveBase + amountIn;
      _reserveQuote = Quantity::fromRaw(_reserveQuote.raw() - out.raw());
    }
    else
    {
      _reserveQuote = _reserveQuote + amountIn;
      _reserveBase = Quantity::fromRaw(_reserveBase.raw() - out.raw());
    }
    return out;
  }

  std::unique_ptr<IAmmCurve> clone() const override
  {
    return std::make_unique<ConstantProductCurve>(*this);
  }

 private:
  Quantity _reserveBase;
  Quantity _reserveQuote;
  int32_t _feeBps;
};

}  // namespace flox
