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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

// Settles option positions at expiry. An option must not linger as an open
// position past its expiry: it realizes intrinsic value and closes.
//
// Cash settlement (crypto / index options, the default): the position closes at
// its intrinsic value, so realized PnL = (intrinsic - premium) * qty *
// multiplier. ITM lands the intrinsic, OTM closes worthless (intrinsic 0, so the
// long simply loses its premium). closePosition already scales by the position's
// contractMultiplier (W16-T017), so no extra scaling here.
//
// Physical settlement (equity) opens the underlying at strike instead, and needs
// an option->underlying symbol mapping; it is out of scope here (W16-T020 / the
// TradFi path) and such symbols are skipped.

namespace flox
{

class OptionSettlementEngine
{
 public:
  explicit OptionSettlementEngine(const SymbolRegistry& registry) : _registry(registry) {}

  // Settle every open option position whose expiry is at or before nowNs, using
  // markFn(symbol) as the underlying / index price for intrinsic. Returns the
  // number of positions settled.
  template <typename MarkFn>
  size_t settleExpired(int64_t nowNs, PositionGroupTracker& positions, MarkFn&& markFn)
  {
    std::vector<std::pair<PositionId, double>> toSettle;

    for (const auto& [pid, pos] : positions.positions())
    {
      if (pos.closed)
      {
        continue;
      }
      auto info = _registry.getSymbolInfo(pos.symbol);
      if (!info || info->type != InstrumentType::Option)
      {
        continue;
      }
      if (!info->expiry.has_value() || !info->strike.has_value() ||
          !info->optionType.has_value())
      {
        continue;
      }
      // Physical settlement needs an underlying mapping (out of scope here).
      if (info->settlementType != SettlementType::Cash)
      {
        continue;
      }

      const int64_t expNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                info->expiry->time_since_epoch())
                                .count();
      if (expNs > nowNs)
      {
        continue;
      }

      const double index = static_cast<double>(markFn(pos.symbol));
      const double strike = info->strike->toDouble();
      const double intrinsic = (*info->optionType == OptionType::CALL)
                                   ? std::max(index - strike, 0.0)
                                   : std::max(strike - index, 0.0);
      toSettle.emplace_back(pid, intrinsic);
    }

    for (const auto& [pid, intrinsic] : toSettle)
    {
      positions.closePosition(pid, Price::fromDouble(intrinsic));
    }
    return toSettle.size();
  }

 private:
  const SymbolRegistry& _registry;
};

}  // namespace flox
