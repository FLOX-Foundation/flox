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
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace flox
{

namespace
{
// Portable 64x64 -> 64 multiply-then-divide via a 128-bit intermediate.
// MSVC lacks __int128, so route through _mul128 / _udiv128 on Windows.
inline int64_t mulDiv64(int64_t a, int64_t b, int64_t c) noexcept
{
#if defined(__SIZEOF_INT128__)
  return static_cast<int64_t>((static_cast<__int128>(a) *
                               static_cast<__int128>(b)) /
                              static_cast<__int128>(c));
#elif defined(_MSC_VER) && defined(_M_X64)
  int64_t hi = 0;
  const int64_t lo = _mul128(a, b, &hi);
  int64_t rem = 0;
  return _div128(hi, lo, c, &rem);
#else
  // Fall back to long double on platforms without a wide multiply
  // primitive. Acceptable here because the result is immediately
  // clamped and re-distributed if it overshoots; we lose only a few
  // raw units at most.
  return static_cast<int64_t>((static_cast<long double>(a) *
                               static_cast<long double>(b)) /
                              static_cast<long double>(c));
#endif
}
}  // namespace

void OrderQueueTracker::setModel(QueueModel model, size_t depth)
{
  _enabled = (model != QueueModel::NONE);
  _model = model;
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

void OrderQueueTracker::compact()
{
  _levels.erase(
      std::remove_if(_levels.begin(), _levels.end(),
                     [](const Level& l)
                     { return l.entries.empty(); }),
      _levels.end());
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
  level.totalQty = levelQtyNow;
  level.entries.push_back(QueueEntry{.orderId = orderId,
                                     .remaining = qty,
                                     .aheadRemaining = levelQtyNow,
                                     .aheadAtArrival = levelQtyNow});
}

void OrderQueueTracker::removeOrder(OrderId orderId)
{
  if (!_enabled)
  {
    return;
  }

  for (auto& level : _levels)
  {
    for (auto it = level.entries.begin(); it != level.entries.end(); ++it)
    {
      if (it->orderId == orderId)
      {
        level.entries.erase(it);
        compact();
        return;
      }
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

  for (auto& level : _levels)
  {
    if (level.key.symbol != symbol || level.key.priceRaw != price.raw())
    {
      continue;
    }

    int64_t remainingRaw = tradeQty.raw();

    // FIFO portion: applies to all entries in NONE/TOB/FULL, and to
    // the first _fifoTopN entries in PRO_RATA_WITH_FIFO. Pure
    // PRO_RATA skips this loop entirely (fifoCount=0).
    const size_t fifoCount =
        (_model == QueueModel::PRO_RATA)             ? 0
        : (_model == QueueModel::PRO_RATA_WITH_FIFO) ? std::min(_fifoTopN, level.entries.size())
                                                     : level.entries.size();
    for (size_t i = 0; i < fifoCount && remainingRaw > 0; ++i)
    {
      auto& entry = level.entries[i];
      const int64_t aheadConsumed = std::min(entry.aheadRemaining.raw(), remainingRaw);
      entry.aheadRemaining = Quantity::fromRaw(entry.aheadRemaining.raw() - aheadConsumed);
      remainingRaw -= aheadConsumed;
      if (remainingRaw <= 0 || entry.aheadRemaining.raw() > 0)
      {
        continue;
      }
      const int64_t fillRaw = std::min(entry.remaining.raw(), remainingRaw);
      if (fillRaw > 0)
      {
        entry.remaining = Quantity::fromRaw(entry.remaining.raw() - fillRaw);
        remainingRaw -= fillRaw;
        filled.emplace_back(entry.orderId, Quantity::fromRaw(fillRaw));
      }
    }

    // Pro-rata portion: distribute remainingRaw across entries from
    // fifoCount onward, weighted by entry.remaining.
    if (remainingRaw > 0 && fifoCount < level.entries.size() &&
        (_model == QueueModel::PRO_RATA || _model == QueueModel::PRO_RATA_WITH_FIFO))
    {
      int64_t totalRest = 0;
      for (size_t i = fifoCount; i < level.entries.size(); ++i)
      {
        totalRest += level.entries[i].remaining.raw();
      }
      if (totalRest > 0)
      {
        const int64_t distributeRaw = std::min(remainingRaw, totalRest);
        int64_t allocated = 0;
        for (size_t i = fifoCount; i < level.entries.size() && allocated < distributeRaw; ++i)
        {
          auto& entry = level.entries[i];
          // Proportional share rounded down to the nearest raw unit.
          int64_t share = mulDiv64(entry.remaining.raw(), distributeRaw, totalRest);
          if (share > entry.remaining.raw())
          {
            share = entry.remaining.raw();
          }
          if (allocated + share > distributeRaw)
          {
            share = distributeRaw - allocated;
          }
          if (share > 0)
          {
            entry.remaining = Quantity::fromRaw(entry.remaining.raw() - share);
            allocated += share;
            filled.emplace_back(entry.orderId, Quantity::fromRaw(share));
          }
        }
        remainingRaw -= allocated;
      }
    }

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

  compact();
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
    level->totalQty = newQty;
    return;
  }

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

void OrderQueueTracker::snapshotAll(std::vector<QueueSnapshot>& out) const
{
  out.clear();
  if (!_enabled)
  {
    return;
  }
  for (const auto& level : _levels)
  {
    for (const auto& entry : level.entries)
    {
      out.push_back(QueueSnapshot{.orderId = entry.orderId,
                                  .ahead = entry.aheadRemaining,
                                  .total = level.totalQty,
                                  .aheadAtArrival = entry.aheadAtArrival});
    }
  }
}

void OrderQueueTracker::trackedPrices(SymbolId symbol, Side side,
                                      std::vector<Price>& out) const
{
  if (!_enabled)
  {
    return;
  }
  for (const auto& level : _levels)
  {
    if (level.key.symbol != symbol || level.key.side != side)
    {
      continue;
    }
    if (level.entries.empty())
    {
      continue;
    }
    out.push_back(Price::fromRaw(level.key.priceRaw));
  }
}

std::optional<QueueSnapshot> OrderQueueTracker::snapshot(OrderId orderId) const
{
  if (!_enabled)
  {
    return std::nullopt;
  }
  for (const auto& level : _levels)
  {
    for (const auto& entry : level.entries)
    {
      if (entry.orderId == orderId)
      {
        return QueueSnapshot{.orderId = entry.orderId,
                             .ahead = entry.aheadRemaining,
                             .total = level.totalQty,
                             .aheadAtArrival = entry.aheadAtArrival};
      }
    }
  }
  return std::nullopt;
}

}  // namespace flox
