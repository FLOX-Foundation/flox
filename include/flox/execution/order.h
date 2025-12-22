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
#include "flox/util/base/time.h"

#include <optional>

namespace flox
{

struct ExecutionFlags
{
  uint8_t reduceOnly : 1 = 0;
  uint8_t closePosition : 1 = 0;
  uint8_t postOnly : 1 = 0;
  uint8_t _reserved : 5 = 0;
};

struct Order
{
  OrderId id{};
  Side side{};
  Price price{};
  Quantity quantity{};
  OrderType type{};
  SymbolId symbol{};

  Quantity filledQuantity{0};

  TimePoint createdAt{};
  std::optional<TimePoint> lastUpdated{};
  std::optional<TimePoint> expiresAfter{};

  std::optional<TimePoint> exchangeTimestamp{};

  // Advanced order fields
  TimeInForce timeInForce{TimeInForce::GTC};
  ExecutionFlags flags{};
  Price triggerPrice{};             // for stop/TP orders
  Price trailingOffset{};           // for trailing stop (fixed)
  int32_t trailingCallbackRate{0};  // for trailing stop (bps, 100 = 1%)

  // Metadata
  uint64_t clientOrderId{0};
  uint16_t strategyId{0};
  uint16_t orderTag{0};  // for OCO grouping

  // Iceberg
  Quantity visibleQuantity{};  // visible size (0 = full)
};

}  // namespace flox
