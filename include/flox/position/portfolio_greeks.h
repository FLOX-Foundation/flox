/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"
#include "flox/engine/symbol_registry.h"
#include "flox/position/position_group.h"
#include "flox/pricing/greeks.h"

#include <array>
#include <chrono>
#include <cstdint>

// Portfolio-level greeks across mixed legs (perp + option on the same or
// different underlyings). For each open position:
//   - option: greeks(type, spot, strike, t, vol) scaled by signed qty * multiplier
//   - perp / future / spot: linear, delta = signed qty * multiplier; no convexity
// "signed" means +1 for a long (BUY) leg and -1 for a short (SELL) leg. A
// delta-neutral hedge of a long call shorts the underlying until net delta ~ 0.
//
// Vega is also bucketed by tenor (<30d, 30-90d, >90d) because vega at different
// expiries is not fungible — a delta/vega-hedged book balances per bucket.

namespace flox
{

struct PortfolioGreeks
{
  double delta{0.0};
  double gamma{0.0};
  double vega{0.0};
  double theta{0.0};  // per year
};

enum class VegaTenor
{
  Short = 0,   // < 30 days
  Medium = 1,  // 30-90 days
  Long = 2     // > 90 days
};

class PortfolioGreeksAggregator
{
 public:
  explicit PortfolioGreeksAggregator(const SymbolRegistry& registry) : _registry(registry) {}

  // Aggregate net greeks over open positions. spotFn(symbol) returns the
  // underlying/index price, volFn(symbol) the implied vol, both for the option
  // legs (ignored for linear legs). nowNs sets time-to-expiry. rate and carry
  // default to 0 (crypto priced off the forward).
  template <typename SpotFn, typename VolFn>
  PortfolioGreeks compute(int64_t nowNs, const PositionGroupTracker& positions, SpotFn&& spotFn,
                          VolFn&& volFn, double rate = 0.0, double carry = 0.0) const
  {
    PortfolioGreeks acc;
    _vegaByTenor = {0.0, 0.0, 0.0};

    for (const auto& [pid, pos] : positions.positions())
    {
      if (pos.closed)
      {
        continue;
      }
      const double sign = (pos.side == Side::SELL) ? -1.0 : 1.0;
      const double scale = sign * pos.quantity.toDouble() * pos.contractMultiplier;

      auto info = _registry.getSymbolInfo(pos.symbol);
      const bool isOption = info && info->type == InstrumentType::Option &&
                            info->strike.has_value() && info->optionType.has_value() &&
                            info->expiry.has_value();

      if (!isOption)
      {
        // Linear leg: delta 1 per unit, no convexity.
        acc.delta += scale;
        continue;
      }

      const int64_t expNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                info->expiry->time_since_epoch())
                                .count();
      const double t = static_cast<double>(expNs - nowNs) / kNsPerYear;
      if (t <= 0.0)
      {
        continue;  // expired; settlement (W16-T018) handles it
      }

      const double spot = static_cast<double>(spotFn(pos.symbol));
      const double vol = static_cast<double>(volFn(pos.symbol));
      const auto g = pricing::greeks(*info->optionType, spot, info->strike->toDouble(), t, rate,
                                     carry, vol);

      acc.delta += scale * g.delta;
      acc.gamma += scale * g.gamma;
      acc.vega += scale * g.vega;
      acc.theta += scale * g.theta;
      _vegaByTenor[static_cast<size_t>(tenorBucket(t))] += scale * g.vega;
    }
    return acc;
  }

  // Net vega in a tenor bucket from the most recent compute() call.
  double vegaInTenor(VegaTenor tenor) const { return _vegaByTenor[static_cast<size_t>(tenor)]; }

  static VegaTenor tenorBucket(double years)
  {
    const double days = years * 365.0;
    if (days < 30.0)
    {
      return VegaTenor::Short;
    }
    if (days <= 90.0)
    {
      return VegaTenor::Medium;
    }
    return VegaTenor::Long;
  }

 private:
  static constexpr double kNsPerYear = 365.0 * 24.0 * 3600.0 * 1e9;

  const SymbolRegistry& _registry;
  mutable std::array<double, 3> _vegaByTenor{0.0, 0.0, 0.0};
};

}  // namespace flox
