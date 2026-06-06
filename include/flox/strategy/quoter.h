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

#include <cmath>
#include <cstdlib>
#include <vector>

namespace flox
{

struct QuoteLevel
{
  Side side;
  Price price;
  Quantity qty;
};

// Two-sided market-making quoter. Produces a ladder of bid and ask quotes
// around a fair price, with a target half-spread, a per-level step, and
// inventory skew. Skew shifts the reservation price against the current
// inventory: a long position pulls the quotes down so sells fill first, a
// short position pulls them up. The quoter only computes the desired ladder;
// the strategy reconciles it with live orders (emit / cancel / modify), using
// shouldRequote to avoid churning on moves smaller than a tolerance.
class Quoter
{
 public:
  // halfSpreadBps: half the target spread in basis points (10 means the level
  //   0 quotes sit 0.10% either side of the reservation price).
  // levels: number of price levels per side.
  // levelSize: quantity per level.
  // levelStepBps: extra offset in bps added for each level beyond the first.
  // inventorySkewBpsPerUnit: how many bps the reservation price moves per unit
  //   of inventory (signed; 0 disables skew).
  Quoter(int32_t halfSpreadBps, int levels, Quantity levelSize, int32_t levelStepBps,
         double inventorySkewBpsPerUnit = 0.0)
      : _halfSpreadBps(halfSpreadBps),
        _levels(levels),
        _levelSize(levelSize),
        _levelStepBps(levelStepBps),
        _skewBpsPerUnit(inventorySkewBpsPerUnit)
  {
  }

  // Fair price shifted against inventory. Long inventory lowers it; short
  // inventory raises it. With zero skew or zero inventory it equals fair.
  Price reservationPrice(Price fair, Quantity inventory) const
  {
    const double skewBps = _skewBpsPerUnit * inventory.toDouble();
    return Price::fromDouble(fair.toDouble() * (1.0 - skewBps / 10000.0));
  }

  // The desired two-sided ladder: levels bids and levels asks, stepped out
  // from the reservation price.
  std::vector<QuoteLevel> quotes(Price fair, Quantity inventory) const
  {
    std::vector<QuoteLevel> out;
    if (_levels <= 0)
    {
      return out;
    }
    out.reserve(static_cast<size_t>(_levels) * 2);
    const double res = reservationPrice(fair, inventory).toDouble();
    for (int i = 0; i < _levels; ++i)
    {
      const double offBps = static_cast<double>(_halfSpreadBps + i * _levelStepBps);
      const double frac = offBps / 10000.0;
      out.push_back({Side::BUY, Price::fromDouble(res * (1.0 - frac)), _levelSize});
      out.push_back({Side::SELL, Price::fromDouble(res * (1.0 + frac)), _levelSize});
    }
    return out;
  }

  // True when a resting quote should be cancel-replaced: the new price has
  // moved more than tolTicks (in Price ticks) from the old one. Lets a
  // strategy hold quotes through small moves instead of churning the book.
  static bool shouldRequote(Price oldPrice, Price newPrice, int64_t tolTicks)
  {
    const int64_t deltaTicks = std::llabs(newPrice.raw() - oldPrice.raw()) / Price::TickSize;
    return deltaTicks > tolTicks;
  }

 private:
  int32_t _halfSpreadBps;
  int _levels;
  Quantity _levelSize;
  int32_t _levelStepBps;
  double _skewBpsPerUnit;
};

}  // namespace flox
