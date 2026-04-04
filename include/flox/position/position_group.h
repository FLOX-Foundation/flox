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

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace flox
{

using PositionId = uint64_t;
using GroupId = uint64_t;

struct IndividualPosition
{
  PositionId positionId{};
  OrderId originOrderId{};
  SymbolId symbol{};
  Side side{};
  Price entryPrice{};
  Quantity quantity{};
  Price realizedPnl{};
  bool closed{false};
  GroupId groupId{0};  // 0 = ungrouped
};

struct PositionGroup
{
  GroupId groupId{};
  GroupId parentGroupId{0};  // 0 = top-level group
  std::vector<PositionId> positionIds;
  std::vector<GroupId> subGroupIds;

  Quantity netPosition(const std::unordered_map<PositionId, IndividualPosition>& positions) const
  {
    int64_t total = 0;
    for (auto pid : positionIds)
    {
      auto it = positions.find(pid);
      if (it == positions.end() || it->second.closed)
      {
        continue;
      }
      int64_t qty = it->second.quantity.raw();
      if (it->second.side == Side::SELL)
      {
        qty = -qty;
      }
      total += qty;
    }
    return Quantity::fromRaw(total);
  }

  Price groupRealizedPnl(const std::unordered_map<PositionId, IndividualPosition>& positions) const
  {
    int64_t total = 0;
    for (auto pid : positionIds)
    {
      auto it = positions.find(pid);
      if (it != positions.end())
      {
        total += it->second.realizedPnl.raw();
      }
    }
    return Price::fromRaw(total);
  }
};

class PositionGroupTracker
{
 public:
  PositionGroupTracker() = default;

  // Open a new individual position from an order fill
  PositionId openPosition(OrderId orderId, SymbolId symbol, Side side,
                          Price entryPrice, Quantity qty)
  {
    PositionId pid = _nextPositionId++;
    IndividualPosition pos{};
    pos.positionId = pid;
    pos.originOrderId = orderId;
    pos.symbol = symbol;
    pos.side = side;
    pos.entryPrice = entryPrice;
    pos.quantity = qty;
    _positions[pid] = pos;
    _orderToPosition[orderId] = pid;
    int64_t signedQty = (side == Side::SELL) ? -qty.raw() : qty.raw();
    _symbolNetQty[symbol] += signedQty;
    return pid;
  }

  // Close (or partially close) a position
  void closePosition(PositionId pid, Price exitPrice)
  {
    auto it = _positions.find(pid);
    if (it == _positions.end())
    {
      return;
    }
    auto& pos = it->second;
    Price priceDiff = exitPrice - pos.entryPrice;
    Volume pnl = pos.quantity * priceDiff;
    if (pos.side == Side::SELL)
    {
      pnl = Volume::fromRaw(-pnl.raw());
    }
    pos.realizedPnl = Price::fromRaw(pos.realizedPnl.raw() + pnl.raw());
    _symbolRealizedPnl[pos.symbol] += pnl.raw();
    int64_t signedQty = (pos.side == Side::SELL) ? -pos.quantity.raw() : pos.quantity.raw();
    _symbolNetQty[pos.symbol] -= signedQty;
    pos.closed = true;
  }

  void partialClose(PositionId pid, Quantity closeQty, Price exitPrice)
  {
    auto it = _positions.find(pid);
    if (it == _positions.end())
    {
      return;
    }
    auto& pos = it->second;
    int64_t closeRaw = std::min(closeQty.raw(), pos.quantity.raw());

    Price priceDiff = exitPrice - pos.entryPrice;
    Volume pnl = Quantity::fromRaw(closeRaw) * priceDiff;
    if (pos.side == Side::SELL)
    {
      pnl = Volume::fromRaw(-pnl.raw());
    }
    pos.realizedPnl = Price::fromRaw(pos.realizedPnl.raw() + pnl.raw());
    _symbolRealizedPnl[pos.symbol] += pnl.raw();
    int64_t signedClose = (pos.side == Side::SELL) ? -closeRaw : closeRaw;
    _symbolNetQty[pos.symbol] -= signedClose;
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() - closeRaw);

    if (pos.quantity.raw() == 0)
    {
      pos.closed = true;
    }
  }

  GroupId createGroup(GroupId parentGroupId = 0)
  {
    GroupId gid = _nextGroupId++;
    PositionGroup group{};
    group.groupId = gid;
    group.parentGroupId = parentGroupId;
    _groups[gid] = group;

    if (parentGroupId != 0)
    {
      auto it = _groups.find(parentGroupId);
      if (it != _groups.end())
      {
        it->second.subGroupIds.push_back(gid);
      }
    }
    return gid;
  }

  bool assignToGroup(PositionId pid, GroupId gid)
  {
    auto pit = _positions.find(pid);
    if (pit == _positions.end())
    {
      return false;
    }
    auto git = _groups.find(gid);
    if (git == _groups.end())
    {
      return false;
    }
    pit->second.groupId = gid;
    git->second.positionIds.push_back(pid);
    return true;
  }

  IndividualPosition* getPosition(PositionId pid)
  {
    auto it = _positions.find(pid);
    return it != _positions.end() ? &it->second : nullptr;
  }

  const IndividualPosition* getPosition(PositionId pid) const
  {
    auto it = _positions.find(pid);
    return it != _positions.end() ? &it->second : nullptr;
  }

  IndividualPosition* getPositionByOrder(OrderId orderId)
  {
    auto it = _orderToPosition.find(orderId);
    if (it == _orderToPosition.end())
    {
      return nullptr;
    }
    return getPosition(it->second);
  }

  const IndividualPosition* getPositionByOrder(OrderId orderId) const
  {
    auto it = _orderToPosition.find(orderId);
    if (it == _orderToPosition.end())
    {
      return nullptr;
    }
    return getPosition(it->second);
  }

  const PositionGroup* getGroup(GroupId gid) const
  {
    auto it = _groups.find(gid);
    return it != _groups.end() ? &it->second : nullptr;
  }

  Quantity netPosition(SymbolId symbol) const
  {
    auto it = _symbolNetQty.find(symbol);
    return it != _symbolNetQty.end() ? Quantity::fromRaw(it->second) : Quantity{};
  }

  void addToNetCache(SymbolId symbol, int64_t signedQtyRaw)
  {
    _symbolNetQty[symbol] += signedQtyRaw;
  }

  Quantity groupNetPosition(GroupId gid) const
  {
    auto it = _groups.find(gid);
    if (it == _groups.end())
    {
      return Quantity{};
    }
    Quantity net = it->second.netPosition(_positions);
    // Include sub-groups recursively
    for (auto subGid : it->second.subGroupIds)
    {
      net = Quantity::fromRaw(net.raw() + groupNetPosition(subGid).raw());
    }
    return net;
  }

  Price realizedPnl(SymbolId symbol) const
  {
    auto it = _symbolRealizedPnl.find(symbol);
    return it != _symbolRealizedPnl.end() ? Price::fromRaw(it->second) : Price{};
  }

  Price totalRealizedPnl() const
  {
    int64_t total = 0;
    for (const auto& [_, pnl] : _symbolRealizedPnl)
    {
      total += pnl;
    }
    return Price::fromRaw(total);
  }

  Price groupRealizedPnl(GroupId gid) const
  {
    auto it = _groups.find(gid);
    if (it == _groups.end())
    {
      return Price{};
    }
    int64_t total = it->second.groupRealizedPnl(_positions).raw();
    for (auto subGid : it->second.subGroupIds)
    {
      total += groupRealizedPnl(subGid).raw();
    }
    return Price::fromRaw(total);
  }

  Volume groupUnrealizedPnl(GroupId gid, Price currentPrice) const
  {
    auto it = _groups.find(gid);
    if (it == _groups.end())
    {
      return Volume{};
    }

    int64_t pnl = 0;
    for (auto pid : it->second.positionIds)
    {
      auto pit = _positions.find(pid);
      if (pit == _positions.end() || pit->second.closed)
      {
        continue;
      }
      const auto& pos = pit->second;
      Price diff = currentPrice - pos.entryPrice;
      Volume posPnl = pos.quantity * diff;
      if (pos.side == Side::SELL)
      {
        posPnl = Volume::fromRaw(-posPnl.raw());
      }
      pnl += posPnl.raw();
    }
    for (auto subGid : it->second.subGroupIds)
    {
      pnl += groupUnrealizedPnl(subGid, currentPrice).raw();
    }
    return Volume::fromRaw(pnl);
  }

  size_t openPositionCount() const
  {
    size_t count = 0;
    for (const auto& [_, pos] : _positions)
    {
      if (!pos.closed)
      {
        ++count;
      }
    }
    return count;
  }

  size_t openPositionCount(SymbolId symbol) const
  {
    size_t count = 0;
    for (const auto& [_, pos] : _positions)
    {
      if (pos.symbol == symbol && !pos.closed)
      {
        ++count;
      }
    }
    return count;
  }

  // Get all open positions for a symbol
  std::vector<const IndividualPosition*> getOpenPositions(SymbolId symbol) const
  {
    std::vector<const IndividualPosition*> result;
    for (const auto& [_, pos] : _positions)
    {
      if (pos.symbol == symbol && !pos.closed)
      {
        result.push_back(&pos);
      }
    }
    return result;
  }

  // Iterate all open positions
  template <typename Func>
  void forEachOpen(Func&& fn) const
  {
    for (const auto& [_, pos] : _positions)
    {
      if (!pos.closed)
      {
        fn(pos);
      }
    }
  }

  void pruneClosedPositions()
  {
    std::unordered_set<PositionId> pruned;
    for (auto it = _positions.begin(); it != _positions.end();)
    {
      if (it->second.closed)
      {
        pruned.insert(it->first);
        _orderToPosition.erase(it->second.originOrderId);
        it = _positions.erase(it);
      }
      else
      {
        ++it;
      }
    }

    if (!pruned.empty())
    {
      for (auto& [_, group] : _groups)
      {
        std::erase_if(group.positionIds, [&](PositionId pid)
                      { return pruned.count(pid); });
      }
    }
  }

 private:
  PositionId _nextPositionId{1};
  GroupId _nextGroupId{1};
  std::unordered_map<PositionId, IndividualPosition> _positions;
  std::unordered_map<OrderId, PositionId> _orderToPosition;
  std::unordered_map<GroupId, PositionGroup> _groups;
  std::unordered_map<SymbolId, int64_t> _symbolNetQty;
  std::unordered_map<SymbolId, int64_t> _symbolRealizedPnl;
};

}  // namespace flox
