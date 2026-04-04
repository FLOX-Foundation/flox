/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>

namespace flox
{

enum class PositionAggregationMode : uint8_t
{
  NET = 0,       // Single net position per symbol (default, broker-style)
  PER_SIDE = 1,  // Separate long/short positions (FX hedging-style)
  GROUPED = 2,   // Each order creates individual position, grouped by contingent orders
};

}  // namespace flox
