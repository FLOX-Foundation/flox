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
  // Post-only: this order may never take. Carried on the resting record because
  // an amend re-enters matching as a fresh aggressor and has to know what the
  // order was admitted with -- the incoming NewOrder is long gone by then.
  bool postOnly{};
  // The identifier the submitter gave this order, carried for the same reason
  // postOnly is: every report about the order is emitted long after the
  // incoming request is gone, and a submitter reconciles against the
  // identifier it chose, not the one the venue assigned. 0 = none was given.
  uint64_t clientOrderId{};
  // Total quantity matched against this order while it rested, accumulated
  // over its whole life on the book (T058: FIX CumQty/14 on a later cancel
  // needs the running total, not just what a single cross just filled).
  // Incremented at the book's two real-fill mutation points only
  // (MatchingBook::fillFront/consumeById, LadderBook::fillBest/consumeById);
  // every other leaves/hidden mutator (reduce, reduceTotal -- amend and STP
  // trims) prints no trade and must not touch this field. 0 = never filled
  // while resting.
  Quantity cumQty{};
};

}  // namespace flox
