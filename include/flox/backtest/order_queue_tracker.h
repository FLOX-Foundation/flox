/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/backtest_config.h"
#include "flox/common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace flox
{

struct QueueEntry
{
  OrderId orderId{};
  Quantity remaining{};       // order qty left to fill
  Quantity aheadRemaining{};  // volume in queue ahead of this order
  Quantity aheadAtArrival{};  // snapshot at registration (for proportional shrink)
};

// Tracks order queue position per price level for limit-order fill simulation.
// TOB mode tracks only the level at which the order was registered (typically
// the current best bid/ask). FULL mode additionally handles movement across
// nearby levels up to queueDepth.
class OrderQueueTracker
{
 public:
  static constexpr size_t kMaxSymbols = 256;

  OrderQueueTracker() = default;

  void setModel(QueueModel model, size_t depth);

  // Register a limit order with its current level depth.
  void addOrder(SymbolId symbol, Side side, Price levelPrice, OrderId orderId,
                Quantity qty, Quantity levelQtyNow);

  // Remove a cancelled order (no-op if unknown).
  void removeOrder(OrderId orderId);

  // Trade at a price level. Consumes queue-ahead first, then fills waiting orders.
  // Appends (orderId, fillQty) pairs to `filled` for each (possibly partial) fill.
  void onTrade(SymbolId symbol, Price price, Quantity tradeQty,
               std::vector<std::pair<OrderId, Quantity>>& filled);

  // A price level changed quantity. Shrinks `aheadRemaining` proportionally
  // when qty decreases (trade-ahead heuristic). Growth adds only behind us.
  void onLevelUpdate(SymbolId symbol, Side side, Price price, Quantity newQty);

  bool enabled() const { return _enabled; }

 private:
  struct LevelKey
  {
    SymbolId symbol{};
    Side side{};
    int64_t priceRaw{0};

    bool operator==(const LevelKey& other) const
    {
      return symbol == other.symbol && side == other.side && priceRaw == other.priceRaw;
    }
  };

  struct Level
  {
    LevelKey key{};
    Quantity totalQty{};  // current level quantity (our fills not counted yet)
    std::vector<QueueEntry> entries;
  };

  struct OrderIndex
  {
    OrderId orderId{};
    size_t levelIdx{0};
  };

  Level* findLevel(const LevelKey& key);
  Level& getOrCreateLevel(const LevelKey& key);

  bool _enabled{false};
  size_t _depth{1};

  std::vector<Level> _levels;
  std::vector<OrderIndex> _orderIndex;
};

}  // namespace flox
