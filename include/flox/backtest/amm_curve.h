/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/common.h"

namespace flox
{

// A two-token (base/quote) AMM pricing curve. Each model (constant-product,
// weighted, concentrated liquidity, stableswap, cryptoswap) implements this so
// the connector, the backtest pricing, and the LP valuator work over any curve
// without knowing which one. The curve holds its own state; baseForQuote=true
// means swapping base into the pool for quote.
class IAmmCurve
{
 public:
  virtual ~IAmmCurve() = default;

  // Marginal price, quote per base.
  virtual Price spotPrice() const = 0;

  // Output amount for swapping amountIn, without mutating state.
  virtual Quantity amountOut(Quantity amountIn, bool baseForQuote) const = 0;

  // Price impact as a fraction in [0, 1): how far the realized rate falls
  // below the spot rate, including fee. Zero for an infinitesimal swap.
  virtual double priceImpact(Quantity amountIn, bool baseForQuote) const = 0;

  // Execute the swap: return the output and move the curve's state along it.
  virtual Quantity applySwap(Quantity amountIn, bool baseForQuote) = 0;
};

}  // namespace flox
