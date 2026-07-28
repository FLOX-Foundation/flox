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

// A single order resting on an order-level matching book (price-time FIFO or
// pro-rata). Distinct from the aggregate NLevelOrderBook, which only tracks
// per-level totals for market data. Shared by every matching-book implementation
// and by the matcher, which holds a RestingOrder* to the current best maker.
struct RestingOrder
{
  OrderId id{};
  uint64_t accountId{};
  Price price{};
  Quantity leaves{};  // currently displayed (executable now)
  Side side{};
  Quantity hidden{};  // iceberg reserve beyond the displayed peak (0 = none)
  Quantity peak{};    // iceberg display size (0 = non-iceberg)
  bool lastLook{};    // maker holds fills for a last-look window before confirming
  bool reduceOnly{};  // perp: may only reduce the account's position, never open/flip
};

}  // namespace flox
