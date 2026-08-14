/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace flox::venue
{

inline bool isConditional(OrderType t) noexcept
{
  return t == OrderType::STOP_MARKET || t == OrderType::STOP_LIMIT ||
         t == OrderType::TAKE_PROFIT_MARKET || t == OrderType::TAKE_PROFIT_LIMIT ||
         t == OrderType::TRAILING_STOP;
}

inline bool isLimitStop(OrderType t) noexcept
{
  return t == OrderType::STOP_LIMIT || t == OrderType::TAKE_PROFIT_LIMIT;
}

inline bool isTakeProfit(OrderType t) noexcept
{
  return t == OrderType::TAKE_PROFIT_MARKET || t == OrderType::TAKE_PROFIT_LIMIT;
}

class StopBook
{
 public:
  bool empty() const noexcept
  {
    return up_.empty() && down_.empty() && trailing_.empty();
  }

  bool contains(OrderId id) const noexcept { return loc_.count(id) != 0; }

  // Owner of a pending conditional order (0 if unknown) -- so a cancel event
  // for a parked stop can be routed to its session.
  uint64_t accountOf(OrderId id) const noexcept
  {
    auto it = loc_.find(id);
    return it == loc_.end() ? 0 : it->second.account;
  }

  // All pending conditional-order ids (for venue-wide emergency cancel).
  std::vector<OrderId> ids() const
  {
    std::vector<OrderId> out;
    out.reserve(loc_.size());
    for (const auto& [id, l] : loc_)
    {
      (void)l;
      out.push_back(id);
    }
    return out;
  }

  // Enumerate every pending conditional order (its NewOrder + current trigger)
  // -- for account snapshots / reconnect reconciliation.
  template <class Fn>
  void forEachPending(Fn&& fn) const
  {
    for (const auto& [t, p] : up_)
    {
      (void)t;
      fn(p.order, p.trigger);
    }
    for (const auto& [t, p] : down_)
    {
      (void)t;
      fn(p.order, p.trigger);
    }
    for (const auto& p : trailing_)
    {
      fn(p.order, p.trigger);
    }
  }

  void add(const NewOrder& o, Price initialTrigger, bool trailing)
  {
    if (trailing)
    {
      trailing_.push_back(Pending{o, initialTrigger});
      loc_[o.id] = Loc{Container::Trailing, initialTrigger, o.accountId};
      return;
    }
    // Up = fires when ref >= trigger (BUY stop-loss, SELL take-profit).
    const bool up = (o.side == Side::BUY) != isTakeProfit(o.type);
    if (up)
    {
      up_.emplace(initialTrigger, Pending{o, initialTrigger});
      loc_[o.id] = Loc{Container::Up, initialTrigger, o.accountId};
    }
    else
    {
      down_.emplace(initialTrigger, Pending{o, initialTrigger});
      loc_[o.id] = Loc{Container::Down, initialTrigger, o.accountId};
    }
  }

  bool cancel(OrderId id)
  {
    auto lit = loc_.find(id);
    if (lit == loc_.end())
    {
      return false;
    }
    const Loc loc = lit->second;
    loc_.erase(lit);
    switch (loc.container)
    {
      case Container::Up:
        eraseFrom(up_, loc.trigger, id);
        return true;
      case Container::Down:
        eraseFrom(down_, loc.trigger, id);
        return true;
      case Container::Trailing:
        for (size_t i = 0; i < trailing_.size(); ++i)
        {
          if (trailing_[i].order.id == id)
          {
            trailing_.erase(trailing_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
          }
        }
        return true;
    }
    return false;
  }

  // Ratchet trailing triggers toward the market; a trailing stop never loosens.
  void updateTrailing(Price last)
  {
    for (auto& s : trailing_)
    {
      const int64_t off = s.order.trailingOffset.raw();
      if (s.order.side == Side::SELL)
      {
        const Price cand = Price::fromRaw(last.raw() - off);
        if (s.trigger.raw() == 0 || cand > s.trigger)
        {
          s.trigger = cand;
        }
      }
      else
      {
        const Price cand = Price::fromRaw(last.raw() + off);
        if (s.trigger.raw() == 0 || cand < s.trigger)
        {
          s.trigger = cand;
        }
      }
    }
  }

  // Pop the highest-priority triggered stop as an aggressor NewOrder. Firing
  // order: most-in-the-money first (largest cross distance), ties by id.
  std::optional<NewOrder> popTriggered(Price ref)
  {
    bool have = false;
    int64_t bestDist = -1;
    OrderId bestId = 0;
    Container bestC = Container::Up;
    Price bestTrig{};

    auto consider = [&](Container c, Price trig, OrderId id, int64_t dist)
    {
      if (!have || dist > bestDist || (dist == bestDist && id < bestId))
      {
        have = true;
        bestDist = dist;
        bestId = id;
        bestC = c;
        bestTrig = trig;
      }
    };

    // Up: fires when ref >= trigger; most crossed = lowest trigger = begin().
    if (!up_.empty() && !(ref < up_.begin()->first))
    {
      const Price trig = up_.begin()->first;
      OrderId id = 0;
      bool first = true;
      for (auto it = up_.lower_bound(trig); it != up_.end() && it->first == trig; ++it)
      {
        if (first || it->second.order.id < id)
        {
          id = it->second.order.id;
          first = false;
        }
      }
      consider(Container::Up, trig, id, ref.raw() - trig.raw());
    }
    // Down: fires when ref <= trigger; most crossed = highest trigger = begin().
    if (!down_.empty() && !(down_.begin()->first < ref))
    {
      const Price trig = down_.begin()->first;
      OrderId id = 0;
      bool first = true;
      for (auto it = down_.lower_bound(trig); it != down_.end() && it->first == trig; ++it)
      {
        if (first || it->second.order.id < id)
        {
          id = it->second.order.id;
          first = false;
        }
      }
      consider(Container::Down, trig, id, trig.raw() - ref.raw());
    }
    // Trailing: small side list, scan for eligible.
    int trailIdx = -1;
    for (size_t i = 0; i < trailing_.size(); ++i)
    {
      const auto& s = trailing_[i];
      if (s.trigger.raw() == 0)
      {
        continue;
      }
      const bool fires = (s.order.side == Side::BUY) ? !(ref < s.trigger) : !(s.trigger < ref);
      if (!fires)
      {
        continue;
      }
      const int64_t dist = (s.order.side == Side::BUY) ? (ref.raw() - s.trigger.raw())
                                                       : (s.trigger.raw() - ref.raw());
      const OrderId id = s.order.id;
      if (!have || dist > bestDist || (dist == bestDist && id < bestId))
      {
        have = true;
        bestDist = dist;
        bestId = id;
        bestC = Container::Trailing;
        trailIdx = static_cast<int>(i);
      }
    }

    if (!have)
    {
      return std::nullopt;
    }

    NewOrder agg;
    if (bestC == Container::Trailing)
    {
      agg = toAggressor(trailing_[static_cast<size_t>(trailIdx)]);
      trailing_.erase(trailing_.begin() + trailIdx);
    }
    else if (bestC == Container::Up)
    {
      auto it = up_.lower_bound(bestTrig);
      for (; it != up_.end() && it->first == bestTrig; ++it)
      {
        if (it->second.order.id == bestId)
        {
          break;
        }
      }
      agg = toAggressor(it->second);
      up_.erase(it);
    }
    else
    {
      auto it = down_.lower_bound(bestTrig);
      for (; it != down_.end() && it->first == bestTrig; ++it)
      {
        if (it->second.order.id == bestId)
        {
          break;
        }
      }
      agg = toAggressor(it->second);
      down_.erase(it);
    }
    loc_.erase(bestId);
    return agg;
  }

 private:
  struct Pending
  {
    NewOrder order{};
    Price trigger{};
  };
  enum class Container : uint8_t
  {
    Up,
    Down,
    Trailing
  };
  struct Loc
  {
    Container container{};
    Price trigger{};
    uint64_t account{};
  };

  static NewOrder toAggressor(const Pending& s)
  {
    NewOrder o = s.order;
    o.type = isLimitStop(s.order.type) ? OrderType::LIMIT : OrderType::MARKET;
    o.visibleQuantity = Quantity{};  // a triggered stop aggresses in full
    return o;
  }

  template <class Map>
  static void eraseFrom(Map& m, Price trigger, OrderId id)
  {
    for (auto it = m.lower_bound(trigger); it != m.end() && it->first == trigger; ++it)
    {
      if (it->second.order.id == id)
      {
        m.erase(it);
        return;
      }
    }
  }

  std::multimap<Price, Pending> up_;                         // ascending: begin() = lowest trigger
  std::multimap<Price, Pending, std::greater<Price>> down_;  // begin() = highest trigger
  std::vector<Pending> trailing_;
  std::unordered_map<OrderId, Loc> loc_;
};

}  // namespace flox::venue
