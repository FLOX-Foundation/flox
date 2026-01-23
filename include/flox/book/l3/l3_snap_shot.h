/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <vector>
#include "flox/common.h"

namespace flox
{
/*
 * L3Snapshot
 *
 * Contract:
 * ----------
 * Represents an authoritative, point-in-time view of all resting orders
 * in a Level-3 order book.
 *
 * This structure is intentionally a plain data container with no behavior.
 * It exists to define the minimal information required to rebuild an
 * L3OrderBook from a clean state (e.g. on startup, recovery, replay, or
 * simulation boundary).
 *
 * Snapshots are produced by upstream systems (exchange feeds, replay engines,
 * simulators, or checkpoint loaders) and are consumed by L3OrderBook via
 * buildFromSnapshot(...).
 *
 * Snapshot creation is not part of the L3OrderBook hot path and is expected
 * to occur infrequently. Use of std::vector is intentional.
 */
struct OrderSnapshot
{
  OrderId id{};
  Price price{};
  Quantity quantity{};
  Side side{};
};

struct L3Snapshot
{
  std::vector<OrderSnapshot> orders_;
};
}  // namespace flox
