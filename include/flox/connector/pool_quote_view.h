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
#include <vector>

namespace flox
{

// A read-only view of an AMM pool's exact curve at the current point on the replay
// timeline. The synthetic book an AmmDexConnector publishes is one discretised view
// of the pool; this is the other, exact one -- a DEX-native strategy (LP / MM
// accounting, impermanent loss, an exact-size taker swap) holds it to price an
// arbitrary size and to read the pool's exact state without going through a book.
//
// A strategy holds it as a plain interface and never has to know the venue is a
// pool. The view always reflects the live curve: as the replay applies swaps and
// checkpoints, quoteOut and reserves move with it.
class IPoolQuoteView
{
 public:
  virtual ~IPoolQuoteView() = default;

  // Exact output (native wei) for swapping amountIn of the in-token at the current
  // state, without moving the pool. baseForQuote=true sells base into the pool.
  virtual u256 quoteOut(const u256& amountIn, bool baseForQuote) const = 0;

  // The pool's exact composition (native wei), indexed [0, curve().tokenCount()). For
  // a concentrated-liquidity pool these are the virtual reserves at the current price.
  virtual const std::vector<u256>& reserves() const = 0;

  // The pool indices this view's base and quote map to.
  virtual std::size_t baseIndex() const = 0;
  virtual std::size_t quoteIndex() const = 0;

  // The exact curve, for venue-specific state a generic view cannot name (a
  // concentrated pool's sqrt price and liquidity, a stable pool's amplification).
  virtual const INTokenCurve& curve() const = 0;
};

}  // namespace flox
