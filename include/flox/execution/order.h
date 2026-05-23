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

// Hold-side hint for exchanges that operate in hedge / dual-position mode
// (Bitget, Bybit, Binance hedge). 0 = unspecified (one-way mode), 1 = long
// side, 2 = short side. Connectors serialise this to whatever field the
// exchange expects (Bitget: posSide=long|short).
enum class HoldSide : uint8_t
{
  Unspecified = 0,
  Long = 1,
  Short = 2
};

struct ExecutionFlags
{
  uint8_t reduceOnly : 1 = 0;
  uint8_t closePosition : 1 = 0;
  uint8_t postOnly : 1 = 0;
  uint8_t holdSide : 2 = 0;  // HoldSide; cast through static_cast<HoldSide>
  uint8_t _reserved : 3 = 0;
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

  // STP account routing. Zero = the default account; the simulator
  // applies self-trade prevention only when two orders share the same
  // accountId. Configure group-level STP via
  // SimulatedExecutor::setSTPGroupMembership to extend the match to
  // every account in the group.
  uint64_t accountId{0};
};

}  // namespace flox
