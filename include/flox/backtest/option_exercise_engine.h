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
#include "flox/pricing/american.h"

#include <cstdint>

// Exercise and assignment for American options. The expiry engine
// (option_settlement_engine.h) handles European-style cash settlement at expiry;
// this handles the early-exercise right an American option carries before then.
//
// Exercising delivers the underlying at the strike and closes the option leg:
//   - Long call  -> buy underlying at strike   (open underlying long)
//   - Long put   -> sell underlying at strike  (open underlying short)
// Assignment is the mirror, forced on a short when the counterparty exercises:
//   - Short call -> sell underlying at strike  (open underlying short)
//   - Short put  -> buy underlying at strike   (open underlying long)
//
// The option leg closes at zero: its premium is already its entry cost, so the
// long realizes -premium and the short keeps +premium, while the option's
// intrinsic value transfers into the underlying position's strike cost basis (no
// double counting). European options have no early-exercise right, so exercise()
// refuses them.

namespace flox
{

class OptionExerciseEngine
{
 public:
  explicit OptionExerciseEngine(const SymbolRegistry& registry) : _registry(registry) {}

  struct ExerciseResult
  {
    bool exercised{false};
    PositionId underlyingPositionId{0};  // valid only when exercised
  };

  // Exercise (long) or assign (short) the option at optionPid, delivering into
  // underlyingSymbol at the strike. Refuses European options, non-options, and
  // already-closed positions (exercised=false). The delivered quantity is the
  // option quantity scaled by its contract multiplier (1 option = multiplier
  // units of underlying).
  ExerciseResult exercise(PositionGroupTracker& positions, PositionId optionPid,
                          SymbolId underlyingSymbol)
  {
    const IndividualPosition* pos = positions.getPosition(optionPid);
    if (!pos || pos->closed)
    {
      return {};
    }
    auto info = _registry.getSymbolInfo(pos->symbol);
    if (!info || info->type != InstrumentType::Option || !info->strike.has_value() ||
        !info->optionType.has_value())
    {
      return {};
    }
    if (info->exerciseStyle != ExerciseStyle::American)
    {
      return {};  // European: no early-exercise right
    }

    const bool isLong = pos->side == Side::BUY;
    const OptionType ot = *info->optionType;
    // Long call / short put -> underlying long; long put / short call -> short.
    const bool underlyingLong = (ot == OptionType::CALL) == isLong;
    const Side underlyingSide = underlyingLong ? Side::BUY : Side::SELL;

    const double strike = info->strike->toDouble();
    // Deliver per the instrument's contract multiplier (1 option = multiplier
    // units of underlying) — the authoritative spec, not the position's copy.
    const double units = pos->quantity.toDouble() * info->contractMultiplier;

    positions.closePosition(optionPid, Price::fromDouble(0.0));
    const PositionId uid = positions.openPosition(
        kSyntheticOrderBase + _exerciseSeq++, underlyingSymbol, underlyingSide,
        Price::fromDouble(strike), Quantity::fromDouble(units), /*contractMultiplier=*/1.0);

    return {true, uid};
  }

  // True when early exercise of a long American option held at `spot` is optimal
  // — spot at or beyond the Barone-Adesi-Whaley critical boundary. European
  // options and zero-time / zero-vol inputs return false.
  bool isEarlyExerciseOptimal(SymbolId optionSymbol, double spot, double t, double rate,
                              double carry, double vol) const
  {
    auto info = _registry.getSymbolInfo(optionSymbol);
    if (!info || info->type != InstrumentType::Option || !info->strike.has_value() ||
        !info->optionType.has_value() || info->exerciseStyle != ExerciseStyle::American)
    {
      return false;
    }
    if (t <= 0.0 || vol <= 0.0)
    {
      return false;
    }
    const double strike = info->strike->toDouble();
    const double crit = pricing::bawCriticalPrice(*info->optionType, strike, t, rate, carry, vol);
    return (*info->optionType == OptionType::CALL) ? spot >= crit : spot <= crit;
  }

 private:
  // Synthetic order ids for exercise-generated underlying positions, parked high
  // to avoid colliding with the caller's real order id space.
  static constexpr OrderId kSyntheticOrderBase = OrderId{1} << 60;

  const SymbolRegistry& _registry;
  uint64_t _exerciseSeq{0};
};

}  // namespace flox
