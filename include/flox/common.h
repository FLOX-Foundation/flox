/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/util/base/decimal.h"

#include <chrono>
#include <cstdint>

namespace flox
{

enum class InstrumentType
{
  Spot,
  Future,
  Option
};

enum class OptionType
{
  CALL,
  PUT
};

enum class OrderType
{
  LIMIT,
  MARKET
};

enum class Side
{
  BUY,
  SELL
};

using SymbolId = uint32_t;
using OrderId = uint64_t;

struct PriceTag
{
};
struct QuantityTag
{
};
struct VolumeTag
{
};

// tick = 0.000001 for everything
using Price = Decimal<PriceTag, 1'000'000, 1>;
using Quantity = Decimal<QuantityTag, 1'000'000, 1>;
using Volume = Decimal<VolumeTag, 1'000'000, 1>;

using TimePoint = std::chrono::steady_clock::time_point;

}  // namespace flox