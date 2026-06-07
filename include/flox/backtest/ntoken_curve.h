/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/util/int/u256.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace flox
{

// An n-token AMM pool, exact and native-wei. This is the single curve interface:
// a two-token pool is just n = 2, so there is no separate 2-token interface.
// Amounts are u256 in the token's own native units (wei), matching what the
// deployed contract computes, so a backtest or pre-trade quote reproduces the
// chain to the wei. Converting to the engine's Quantity happens at the boundary
// where a curve result becomes an engine event (the connector), not on the
// curve.
//
// Tokens are indexed [0, tokenCount). A swap names the in-token i and out-token
// j. The exact integer math (floor division, the contract's rounding) is the
// whole point; there is no double approximation behind this.
class INTokenCurve
{
 public:
  virtual ~INTokenCurve() = default;

  // Number of tokens in the pool.
  virtual std::size_t tokenCount() const = 0;

  // Per-token balances (native wei), indexed [0, tokenCount). Lets generic code
  // value the pool without the concrete type.
  virtual const std::vector<u256>& balances() const = 0;

  // Output of token j (native wei) for swapping amountIn of token i, exact, no
  // state change.
  virtual u256 amountOut(std::size_t i, std::size_t j, const u256& amountIn) const = 0;

  // Execute the swap: return the output of token j and move the pool state.
  virtual u256 applySwap(std::size_t i, std::size_t j, const u256& amountIn) = 0;

  // Independent deep copy, for sizing a swap to a target without mutating the
  // live pool.
  virtual std::unique_ptr<INTokenCurve> clone() const = 0;
};

}  // namespace flox
