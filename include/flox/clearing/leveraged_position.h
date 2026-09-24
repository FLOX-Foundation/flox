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

#include <cstdint>

namespace flox
{

// One margined position on an account. Side is carried in the sign of
// `quantity`; equity is the residual account balance backing this position,
// and a liquidation that burns through it leaves a deficit for whoever runs
// the insurance fund.
//
// Declared here rather than in flox/backtest/liquidation_engine.h, where it
// used to live: Account holds a vector of these, the venue's clearing reads
// an Account, and a venue has no business compiling a backtest's liquidation
// engine to learn the shape of a position. The liquidation engine includes
// this header and is unchanged otherwise.
// Every field that carries a number is fixed point. Margin and liquidation are
// the last place in the engine that should be computing in double: a double
// quantity cannot even represent the fills that produced it, so every notional,
// maintenance-margin requirement and liquidation price derived from one starts
// from a value that is already approximate, and by a compiler-dependent amount.
struct LeveragedPosition
{
  uint64_t accountId{0};
  SymbolId symbol{};
  Quantity quantity{};  // signed: + long, - short
  Price entryPrice{};
  Volume equity{};  // margin posted backing this position
  // Notional / PnL scale (options 100, ES 50; perp 1.0). A multiplier is a
  // count of units per contract, which is what Quantity spells.
  Quantity contractMultiplier{Quantity::fromRaw(Quantity::Scale)};
  bool isLongOption{false};  // premium-paid long option: not margined, max loss = premium
};

}  // namespace flox
