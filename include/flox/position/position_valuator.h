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

// Hook for valuing a position. The default is linear:
// qty * (current - avgEntry). A position whose value is a nonlinear function
// of price (an AMM LP position with impermanent loss and accrued fees, an
// option, a convex payoff) supplies its own valuator. The valuator receives
// the symbol so a stateful implementation can key its own per-position data
// (range, liquidity, fees) by symbol.
class IPositionValuator
{
 public:
  virtual ~IPositionValuator() = default;

  virtual Volume unrealizedPnl(SymbolId symbol, Quantity qty, Price avgEntry,
                               Price currentPrice) const = 0;
};

// Default linear valuation, used when no custom valuator is set. Reproduces
// the prior hardcoded behavior exactly, so spot and perp positions are
// unchanged.
class LinearPositionValuator : public IPositionValuator
{
 public:
  Volume unrealizedPnl(SymbolId /*symbol*/, Quantity qty, Price avgEntry,
                       Price currentPrice) const override
  {
    Price diff = currentPrice - avgEntry;
    return qty * diff;
  }
};

}  // namespace flox
