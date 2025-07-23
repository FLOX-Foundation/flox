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

#include <chrono>
#include <optional>

namespace flox
{

enum class OrderStatus
{
  NEW,
  PENDING,
  PARTIALLY_FILLED,
  FILLED,
  CANCELED,
  EXPIRED,
  REPLACED,
  REJECTED
};

struct Order
{
  OrderId id{};
  Side side{};
  Price price{};
  Quantity quantity{};
  OrderType type{};
  SymbolId symbol{};

  OrderStatus status = OrderStatus::NEW;
  Quantity filledQuantity{0};

  TimePoint createdAt{};
  std::optional<TimePoint> exchangeTimestamp;
  std::optional<TimePoint> lastUpdated;
  std::optional<TimePoint> expiresAfter;
};

}  // namespace flox