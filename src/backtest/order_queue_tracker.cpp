/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/order_queue_tracker.h"

#include <algorithm>

namespace flox
{

void OrderQueueTracker::setModel(QueueModel model, size_t depth)
{
  _enabled = (model != QueueModel::NONE);
  _depth = (model == QueueModel::FULL) ? std::max<size_t>(depth, 1) : 1;
}

OrderQueueTracker::Level* OrderQueueTracker::findLevel(const LevelKey& key)
{
  for (auto& level : _levels)
  {
    if (level.key == key)
    {
      return &level;
    }
  }
  return nullptr;
}

OrderQueueTracker::Level& OrderQueueTracker::getOrCreateLevel(const LevelKey& key)
{
  if (Level* existing = findLevel(key))
  {
    return *existing;
  }
  _levels.push_back(Level{.key = key, .totalQty = Quantity{}, .entries = {}});
  return _levels.back();
}

void OrderQueueTracker::addOrder(SymbolId symbol, Side side, Price levelPrice,
                                 OrderId orderId, Quantity qty, Quantity levelQtyNow)
{
  if (!_enabled)
  {
    return;
  }

  LevelKey key{.symbol = symbol, .side = side, .priceRaw = levelPrice.raw()};
  Level& level = getOrCreateLevel(key);
  // levelQtyNow is the full posted volume at arrival; we treat all of it as
  // "ahead of us" (our order joins the back of the queue).
  level.totalQty = levelQtyNow;
  level.entries.push_back(QueueEntry{.orderId = orderId,
                                     .remaining = qty,
                                     .aheadRemaining = levelQtyNow,
                                     .aheadAtArrival = levelQtyNow});

  _orderIndex.push_back(OrderIndex{.orderId = orderId,
                                   .levelIdx = static_cast<size_t>(&level - _levels.data())});
}

void OrderQueueTracker::removeOrder(OrderId orderId)
{
  if (!_enabled)
  {
    return;
  }

  for (auto it = _orderIndex.begin(); it != _orderIndex.end(); ++it)
  {
    if (it->orderId == orderId)
    {
      Level& level = _levels[it->levelIdx];
      for (auto eit = level.entries.begin(); eit != level.entries.end(); ++eit)
      {
        if (eit->orderId == orderId)
        {
          level.entries.erase(eit);
          break;
        }
      }
      _orderIndex.erase(it);
      return;
    }
  }
}

void OrderQueueTracker::onTrade(SymbolId symbol, Price price, Quantity tradeQty,
                                std::vector<std::pair<OrderId, Quantity>>& filled)
{
  if (!_enabled || tradeQty.raw() <= 0)
  {
    return;
  }

  // Trades can match at either side's level (taker buys consume asks, taker
  // sells consume bids). Walk both sides at this price.
  for (auto& level : _levels)
  {
    if (level.key.symbol != symbol || level.key.priceRaw != price.raw())
    {
      continue;
    }

    int64_t remainingRaw = tradeQty.raw();
    for (auto& entry : level.entries)
    {
      if (remainingRaw <= 0)
      {
        break;
      }

      // First consume queue ahead
      const int64_t aheadConsumed = std::min(entry.aheadRemaining.raw(), remainingRaw);
      entry.aheadRemaining = Quantity::fromRaw(entry.aheadRemaining.raw() - aheadConsumed);
      remainingRaw -= aheadConsumed;

      if (remainingRaw <= 0 || entry.aheadRemaining.raw() > 0)
      {
        continue;
      }

      // Queue ahead exhausted, fill our order
      const int64_t fillRaw = std::min(entry.remaining.raw(), remainingRaw);
      if (fillRaw > 0)
      {
        entry.remaining = Quantity::fromRaw(entry.remaining.raw() - fillRaw);
        remainingRaw -= fillRaw;
        filled.emplace_back(entry.orderId, Quantity::fromRaw(fillRaw));
      }
    }

    // Drop fully-filled entries and refresh totalQty estimate
    level.entries.erase(std::remove_if(level.entries.begin(), level.entries.end(),
                                       [](const QueueEntry& e)
                                       { return e.remaining.raw() <= 0; }),
                        level.entries.end());
    if (level.totalQty.raw() > tradeQty.raw())
    {
      level.totalQty = Quantity::fromRaw(level.totalQty.raw() - tradeQty.raw());
    }
    else
    {
      level.totalQty = Quantity::fromRaw(0);
    }
  }

  // Compact order index by removing filled entries
  _orderIndex.erase(std::remove_if(_orderIndex.begin(), _orderIndex.end(),
                                   [this](const OrderIndex& oi)
                                   {
                                     const Level& l = _levels[oi.levelIdx];
                                     for (const auto& e : l.entries)
                                     {
                                       if (e.orderId == oi.orderId)
                                       {
                                         return false;
                                       }
                                     }
                                     return true;
                                   }),
                    _orderIndex.end());
}

void OrderQueueTracker::onLevelUpdate(SymbolId symbol, Side side, Price price,
                                      Quantity newQty)
{
  if (!_enabled)
  {
    return;
  }

  LevelKey key{.symbol = symbol, .side = side, .priceRaw = price.raw()};
  Level* level = findLevel(key);
  if (!level)
  {
    return;
  }

  const int64_t oldQtyRaw = level->totalQty.raw();
  const int64_t newQtyRaw = newQty.raw();

  if (newQtyRaw >= oldQtyRaw)
  {
    // Level grew: new liquidity joins behind us. No change to aheadRemaining.
    level->totalQty = newQty;
    return;
  }

  // Level shrank. onTrade already consumed the "trade-ahead" portion; any
  // leftover shrink is assumed to be cancels, which remove queue ahead of us
  // proportionally to each entry's remaining aheadRemaining.
  const int64_t shrinkRaw = oldQtyRaw - newQtyRaw;
  int64_t totalAhead = 0;
  for (const auto& entry : level->entries)
  {
    totalAhead += entry.aheadRemaining.raw();
  }

  if (totalAhead > 0)
  {
    for (auto& entry : level->entries)
    {
      const int64_t reduce =
          (entry.aheadRemaining.raw() * shrinkRaw) / totalAhead;
      const int64_t newAhead = entry.aheadRemaining.raw() - reduce;
      entry.aheadRemaining = Quantity::fromRaw(std::max<int64_t>(newAhead, 0));
    }
  }

  level->totalQty = newQty;
}

}  // namespace flox
