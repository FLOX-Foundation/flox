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

#include <cstddef>
#include <memory>
#include <vector>

namespace flox
{

// An n-token AMM pool that prices and swaps between any pair of its tokens.
// This is the multi-asset sibling of IAmmCurve: IAmmCurve is two-token
// (base/quote, a baseForQuote bool), which covers every family in pair form,
// while a pool that does not reduce to a pair -- a Balancer weighted basket, a
// Curve n-coin stable pool, a tricrypto pool -- prices between an ordered pair
// of token indices instead. The two interfaces coexist; IAmmCurve is not
// reimplemented on top of this.
//
// Tokens are indexed [0, tokenCount). A swap names the in-token i and the
// out-token j; price is always quote-per-base in the sense of "units of token i
// per unit of token j" for spotPrice(i, j). The pool holds its own state.
class INTokenCurve
{
 public:
  virtual ~INTokenCurve() = default;

  // Number of tokens in the pool.
  virtual std::size_t tokenCount() const = 0;

  // Per-token balances the pool holds, indexed [0, tokenCount). Lets generic
  // code value the pool (LP marking, IL accounting) without the concrete type.
  virtual const std::vector<double>& balances() const = 0;

  // Marginal price of token j expressed in token i: how much i one unit of j is
  // worth at the current state.
  virtual Price spotPrice(std::size_t i, std::size_t j) const = 0;

  // Output amount of token j for swapping amountIn of token i, no state change.
  virtual Quantity amountOut(std::size_t i, std::size_t j, Quantity amountIn) const = 0;

  // Price impact as a fraction in [0, 1): how far the realized rate falls below
  // the spot rate, including fee. Zero for an infinitesimal swap.
  virtual double priceImpact(std::size_t i, std::size_t j, Quantity amountIn) const = 0;

  // Execute the swap: return the output of token j and move the pool state.
  virtual Quantity applySwap(std::size_t i, std::size_t j, Quantity amountIn) = 0;

  // Independent deep copy, for sizing a swap to a target price without mutating
  // the live pool (same rationale as IAmmCurve::clone).
  virtual std::unique_ptr<INTokenCurve> clone() const = 0;
};

}  // namespace flox
