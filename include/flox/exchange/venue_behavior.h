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

// True for venues that settle on-chain (AMM and hybrid DEXes).
constexpr bool isDex(VenueType v)
{
  return v == VenueType::AmmDex || v == VenueType::HybridDex;
}

// The behavior a venue type implies. This is the switch that turns the DEX
// pieces on per venue: usesOrderBook selects CLOB matching versus AMM
// reserve pricing, and onChainSettlement selects the synchronous CEX order
// lifecycle versus the probabilistic on-chain lifecycle (pending / reverted /
// gas-replaced). Routing code reads these flags instead of branching on the
// enum directly, so a new venue type is wired in one place.
struct VenueBehavior
{
  bool usesOrderBook;      // CLOB matching and book-based pricing
  bool onChainSettlement;  // probabilistic, revertible settlement
};

constexpr VenueBehavior venueBehavior(VenueType v)
{
  switch (v)
  {
    case VenueType::CentralizedExchange:
      return {true, false};
    case VenueType::AmmDex:
      return {false, true};
    case VenueType::HybridDex:
      // An order book front-end that settles on-chain.
      return {true, true};
  }
  return {true, false};
}

}  // namespace flox
