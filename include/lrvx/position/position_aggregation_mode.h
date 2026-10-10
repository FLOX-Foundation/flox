/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>

namespace lrvx
{

enum class PositionAggregationMode : uint8_t
{
  NET = 0,       // Single net position per symbol (default, broker-style)
  PER_SIDE = 1,  // Separate long/short positions (FX hedging-style)
  GROUPED = 2,   // Each order creates individual position, grouped by contingent orders
};

}  // namespace lrvx
