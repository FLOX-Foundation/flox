/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/common.h"
#include "lrvx/execution/order.h"

#include <cstdint>

namespace lrvx
{

// Per-leg description of a bracket order. Keep the data minimal:
// the executor fills in id / createdAt / time-in-force from the
// surrounding submit call.
struct BracketLeg
{
  Side side{};
  OrderType type{OrderType::LIMIT};
  Price price{};
  Price triggerPrice{};  // for stop / take-profit triggers
  Quantity quantity{};
};

// A bracket order: an entry leg that arms a take-profit + stop pair
// on (partial or full) entry fill. The first child to fill cancels
// the other. The simulator manages the state machine; strategies
// don't need to wire OCO / OrderGroup directly.
struct BracketOrder
{
  uint64_t bracketId{0};
  SymbolId symbol{};
  BracketLeg entry{};
  BracketLeg takeProfit{};
  BracketLeg stop{};
};

// Per-bracket state observable from the executor. Useful for
// diagnostics, dashboards, replay-trace inspection.
enum class BracketState : uint8_t
{
  PENDING_ENTRY = 0,  // entry submitted, not filled
  ENTRY_FILLED = 1,   // entry fully filled, children armed
  TP_FILLED = 2,      // take-profit filled, stop cancelled
  STOP_FILLED = 3,    // stop triggered/filled, take-profit cancelled
  CANCELED = 4,       // bracket cancelled before children resolved
};

struct BracketStatus
{
  uint64_t bracketId{0};
  BracketState state{BracketState::PENDING_ENTRY};
  OrderId entryOrderId{0};
  OrderId tpOrderId{0};
  OrderId stopOrderId{0};
  Quantity entryFilled{};
};

}  // namespace lrvx
