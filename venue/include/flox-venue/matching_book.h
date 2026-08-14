/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Order-level resting book with price-time (FIFO) priority. Distinct from the
 * aggregate NLevelOrderBook (market-data depth): this tracks individual orders
 * so it can match, cancel by id, and refill icebergs.
 *
 * REFERENCE ORACLE, not a production book. Correctness-first std::map +
 * std::list; allocates on the hot path by design. Its job is to be the
 * obviously-correct baseline the venue matcher is verified against:
 * test_venue_differential_fuzz drives this book and flox::LadderBook (the
 * fixed-capacity, zero-alloc replacement in flox/book/ladder_book.h) through
 * the same order flow and requires identical event streams. Use LadderBook for
 * anything latency-sensitive; both plug into the same Book seam of
 * MatchingEngine/SequencedShard/SymbolRouter.
 */
#pragma once

#include "flox/book/resting_order.h"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace flox
{

class MatchingBook
{
 public:
  bool contains(OrderId id) const noexcept { return index_.count(id) != 0; }

  bool empty() const noexcept { return bids_.empty() && asks_.empty(); }

  void addResting(Side side, const RestingOrder& o)
  {
    auto& lst = (side == Side::BUY) ? bids_[o.price] : asks_[o.price];
    lst.push_back(o);
    lst.back().side = side;
    index_[o.id] = Loc{side, o.price, std::prev(lst.end())};
  }

  const RestingOrder* find(OrderId id) const
  {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &*it->second.pos;
  }

  // In-place leaves reduction (keeps FIFO position). Caller guarantees
  // newLeaves <= current and > 0.
  bool reduce(OrderId id, Quantity newLeaves)
  {
    auto it = index_.find(id);
    if (it == index_.end())
    {
      return false;
    }
    it->second.pos->leaves = newLeaves;
    return true;
  }

  // Reduce an order (by id) by `by`, with iceberg refill+requeue when its peak
  // is exhausted, else remove it. Used by pro-rata matching.
  void consumeById(OrderId id, Quantity by)
  {
    auto iit = index_.find(id);
    if (iit == index_.end())
    {
      return;
    }
    const Loc loc = iit->second;
    RestingOrder& order = *loc.pos;
    order.leaves -= by;
    if (!order.leaves.isZero())
    {
      return;
    }
    if (!order.hidden.isZero())
    {
      const Quantity newVis = (order.peak < order.hidden) ? order.peak : order.hidden;
      order.leaves = newVis;
      order.hidden = order.hidden - newVis;
      auto& lst = (loc.side == Side::BUY) ? bids_[loc.price] : asks_[loc.price];
      lst.splice(lst.end(), lst, loc.pos);  // requeue to tail; iterator stays valid
      return;
    }
    if (loc.side == Side::BUY)
    {
      auto lvl = bids_.find(loc.price);
      lvl->second.erase(loc.pos);
      if (lvl->second.empty())
      {
        bids_.erase(lvl);
      }
    }
    else
    {
      auto lvl = asks_.find(loc.price);
      lvl->second.erase(loc.pos);
      if (lvl->second.empty())
      {
        asks_.erase(lvl);
      }
    }
    index_.erase(iit);
  }

  std::optional<RestingOrder> cancel(OrderId id)
  {
    auto it = index_.find(id);
    if (it == index_.end())
    {
      return std::nullopt;
    }
    const Loc loc = it->second;
    const RestingOrder copy = *loc.pos;

    if (loc.side == Side::BUY)
    {
      auto lvl = bids_.find(loc.price);
      lvl->second.erase(loc.pos);
      if (lvl->second.empty())
      {
        bids_.erase(lvl);
      }
    }
    else
    {
      auto lvl = asks_.find(loc.price);
      lvl->second.erase(loc.pos);
      if (lvl->second.empty())
      {
        asks_.erase(lvl);
      }
    }
    index_.erase(it);
    return copy;
  }

  // Enumerate every resting order in canonical book order: bids best-first,
  // then asks best-first, FIFO within each level. Used by the venue checkpoint
  // serializer and state hash (not a hot path); the canonical order is what
  // makes a snapshot file deterministic and tail-appending restores exact.
  template <class Fn>
  void forEachOrder(Fn&& fn) const
  {
    for (const auto& [p, lst] : bids_)
    {
      (void)p;
      for (const auto& o : lst)
      {
        fn(o);
      }
    }
    for (const auto& [p, lst] : asks_)
    {
      (void)p;
      for (const auto& o : lst)
      {
        fn(o);
      }
    }
  }

  // Aggregated (price, total qty) per level, best-first (bids descending, asks
  // ascending). Used by the auction uncross.
  void levels(Side side, std::vector<std::pair<Price, Quantity>>& out) const
  {
    out.clear();
    if (side == Side::BUY)
    {
      for (const auto& [p, lst] : bids_)
      {
        Quantity q{};
        for (const auto& o : lst)
        {
          q += o.leaves + o.hidden;
        }
        out.emplace_back(p, q);
      }
    }
    else
    {
      for (const auto& [p, lst] : asks_)
      {
        Quantity q{};
        for (const auto& o : lst)
        {
          q += o.leaves + o.hidden;
        }
        out.emplace_back(p, q);
      }
    }
  }

  // Copy all orders at the best price of `restingSide` in FIFO order (pro-rata
  // matching enumerates the whole level).
  void bestLevel(Side restingSide, std::vector<RestingOrder>& out) const
  {
    out.clear();
    if (restingSide == Side::BUY)
    {
      if (!bids_.empty())
      {
        for (const auto& o : bids_.begin()->second)
        {
          out.push_back(o);
        }
      }
    }
    else
    {
      if (!asks_.empty())
      {
        for (const auto& o : asks_.begin()->second)
        {
          out.push_back(o);
        }
      }
    }
  }

  // Head order (FIFO) at the best price of `restingSide`, or nullptr.
  RestingOrder* peekBest(Side restingSide) noexcept
  {
    if (restingSide == Side::BUY)
    {
      if (bids_.empty())
      {
        return nullptr;
      }
      auto& lst = bids_.begin()->second;
      return lst.empty() ? nullptr : &lst.front();
    }
    if (asks_.empty())
    {
      return nullptr;
    }
    auto& lst = asks_.begin()->second;
    return lst.empty() ? nullptr : &asks_.begin()->second.front();
  }

  // Reduce the best head by `by`; drop it (and its level) when depleted.
  void fillBest(Side restingSide, Quantity by)
  {
    if (restingSide == Side::BUY)
    {
      fillFront(bids_, by);
    }
    else
    {
      fillFront(asks_, by);
    }
  }

  std::optional<Price> bestBid() const noexcept
  {
    if (bids_.empty())
    {
      return std::nullopt;
    }
    return bids_.begin()->first;
  }

  std::optional<Price> bestAsk() const noexcept
  {
    if (asks_.empty())
    {
      return std::nullopt;
    }
    return asks_.begin()->first;
  }

  // Total resting quantity a taker of `takerSide` could sweep at prices that
  // cross `limit` (or the whole opposite side when `isMarket`). Used for the
  // FOK all-or-none precheck.
  Quantity availableWithin(Side takerSide, Price limit, bool isMarket) const
  {
    return availableWithinExcl(takerSide, limit, isMarket, [](const RestingOrder&)
                               { return false; });
  }

  // availableWithin, skipping makers for which skip(order) is true. An STP-aware
  // FOK precheck uses this: same-STP-scope liquidity can never fill the taker (it
  // is canceled/decremented, not traded), so it must not count toward "can this
  // order fully fill" -- otherwise a FOK+STP rests instead of being killed. The
  // predicate sees the whole resting order so non-firm (last-look) liquidity can
  // be excluded the same way.
  template <class Skip>
  Quantity availableWithinExcl(Side takerSide, Price limit, bool isMarket, Skip skip) const
  {
    Quantity total{};
    if (takerSide == Side::BUY)
    {
      for (const auto& [px, lst] : asks_)
      {
        if (!isMarket && limit < px)
        {
          break;
        }
        for (const auto& o : lst)
        {
          if (!skip(o))
          {
            total += o.leaves + o.hidden;  // hidden reserve is real liquidity
          }
        }
      }
    }
    else
    {
      for (const auto& [px, lst] : bids_)
      {
        if (!isMarket && px < limit)
        {
          break;
        }
        for (const auto& o : lst)
        {
          if (!skip(o))
          {
            total += o.leaves + o.hidden;
          }
        }
      }
    }
    return total;
  }

 private:
  struct Loc
  {
    Side side{};
    Price price{};
    std::list<RestingOrder>::iterator pos{};
  };

  template <class Map>
  void fillFront(Map& m, Quantity by)
  {
    auto lvl = m.begin();
    auto& lst = lvl->second;
    auto& head = lst.front();
    head.leaves -= by;
    if (head.leaves.isZero())
    {
      if (!head.hidden.isZero())
      {
        // iceberg: expose the next peak, re-queue at the tail (lose priority).
        const Quantity newVis = (head.peak < head.hidden) ? head.peak : head.hidden;
        head.leaves = newVis;
        head.hidden = head.hidden - newVis;
        lst.splice(lst.end(), lst, lst.begin());  // splice preserves the index_ iterator
      }
      else
      {
        index_.erase(head.id);
        lst.pop_front();
        if (lst.empty())
        {
          m.erase(lvl);
        }
      }
    }
  }

  // bids descending (best = highest), asks ascending (best = lowest)
  std::map<Price, std::list<RestingOrder>, std::greater<Price>> bids_;
  std::map<Price, std::list<RestingOrder>, std::less<Price>> asks_;
  std::unordered_map<OrderId, Loc> index_;
};

}  // namespace flox
