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
  Inverse,
  Option
};

enum class OptionType
{
  CALL,
  PUT
};

enum class OrderType : uint8_t
{
  LIMIT = 0,
  MARKET = 1,
  STOP_MARKET = 2,
  STOP_LIMIT = 3,
  TAKE_PROFIT_MARKET = 4,
  TAKE_PROFIT_LIMIT = 5,
  TRAILING_STOP = 6,
  ICEBERG = 7,
};

enum class TimeInForce : uint8_t
{
  GTC = 0,        // Good Till Cancel (default)
  IOC = 1,        // Immediate Or Cancel
  FOK = 2,        // Fill Or Kill
  GTD = 3,        // Good Till Date
  POST_ONLY = 4,  // Maker only
};

enum class Side
{
  BUY,
  SELL
};

using SymbolId = uint32_t;
using OrderId = uint64_t;
using ExchangeId = uint16_t;

static constexpr ExchangeId InvalidExchangeId = 0xFFFF;

enum class VenueType : uint8_t
{
  CentralizedExchange,
  AmmDex,
  HybridDex
};

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

}  // namespace flox
