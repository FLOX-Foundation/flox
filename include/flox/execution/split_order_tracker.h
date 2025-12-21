/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/common.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace flox
{

class SplitOrderTracker
{
 public:
  static constexpr size_t kMaxChildrenPerSplit = 8;

  struct SplitState
  {
    OrderId parentId{};
    std::array<OrderId, kMaxChildrenPerSplit> childIds{};
    std::array<ExchangeId, kMaxChildrenPerSplit> childExchanges{};
    uint8_t childCount{0};
    uint8_t completedCount{0};
    uint8_t failedCount{0};
    int64_t totalQtyRaw{0};
    int64_t filledQtyRaw{0};
    int64_t createdAtNs{0};

    bool allDone() const
    {
      return completedCount + failedCount >= childCount;
    }

    bool allSuccess() const { return completedCount >= childCount; }

    double fillRatio() const
    {
      if (totalQtyRaw == 0)
      {
        return 0.0;
      }
      return static_cast<double>(filledQtyRaw) / static_cast<double>(totalQtyRaw);
    }
  };

  bool registerSplit(OrderId parent,
                     std::span<const OrderId> children,
                     std::span<const ExchangeId> exchanges,
                     int64_t totalQtyRaw,
                     int64_t nowNs)
  {
    if (children.size() > kMaxChildrenPerSplit)
    {
      return false;
    }
    if (children.size() != exchanges.size())
    {
      return false;
    }
    if (_parentStates.contains(parent))
    {
      return false;
    }

    SplitState state{};
    state.parentId = parent;
    state.childCount = static_cast<uint8_t>(children.size());
    state.totalQtyRaw = totalQtyRaw;
    state.createdAtNs = nowNs;

    for (size_t i = 0; i < children.size(); ++i)
    {
      state.childIds[i] = children[i];
      state.childExchanges[i] = exchanges[i];
      _childToParent[children[i]] = parent;
    }

    _parentStates[parent] = state;
    return true;
  }

  bool registerSplit(OrderId parent,
                     std::span<const OrderId> children,
                     int64_t totalQtyRaw,
                     int64_t nowNs)
  {
    if (children.size() > kMaxChildrenPerSplit)
    {
      return false;
    }
    if (_parentStates.contains(parent))
    {
      return false;
    }

    SplitState state{};
    state.parentId = parent;
    state.childCount = static_cast<uint8_t>(children.size());
    state.totalQtyRaw = totalQtyRaw;
    state.createdAtNs = nowNs;

    for (size_t i = 0; i < children.size(); ++i)
    {
      state.childIds[i] = children[i];
      state.childExchanges[i] = InvalidExchangeId;
      _childToParent[children[i]] = parent;
    }

    _parentStates[parent] = state;
    return true;
  }

  void onChildFill(OrderId childId, int64_t fillQtyRaw)
  {
    auto it = _childToParent.find(childId);
    if (it == _childToParent.end())
    {
      return;
    }

    auto pit = _parentStates.find(it->second);
    if (pit == _parentStates.end())
    {
      return;
    }

    pit->second.filledQtyRaw += fillQtyRaw;
  }

  void onChildComplete(OrderId childId, bool success)
  {
    auto it = _childToParent.find(childId);
    if (it == _childToParent.end())
    {
      return;
    }

    auto pit = _parentStates.find(it->second);
    if (pit == _parentStates.end())
    {
      return;
    }

    if (success)
    {
      ++pit->second.completedCount;
    }
    else
    {
      ++pit->second.failedCount;
    }
  }

  OrderId getParent(OrderId childId) const
  {
    auto it = _childToParent.find(childId);
    return it != _childToParent.end() ? it->second : 0;
  }

  const SplitState* getState(OrderId parentId) const
  {
    auto it = _parentStates.find(parentId);
    return it != _parentStates.end() ? &it->second : nullptr;
  }

  bool isComplete(OrderId parentId) const
  {
    auto* state = getState(parentId);
    return state && state->allDone();
  }

  bool isSuccessful(OrderId parentId) const
  {
    auto* state = getState(parentId);
    return state && state->allSuccess();
  }

  void remove(OrderId parentId)
  {
    auto it = _parentStates.find(parentId);
    if (it == _parentStates.end())
    {
      return;
    }

    for (uint8_t i = 0; i < it->second.childCount; ++i)
    {
      _childToParent.erase(it->second.childIds[i]);
    }
    _parentStates.erase(it);
  }

  void cleanup(int64_t nowNs, int64_t timeoutNs)
  {
    for (auto it = _parentStates.begin(); it != _parentStates.end();)
    {
      if (nowNs - it->second.createdAtNs > timeoutNs)
      {
        for (uint8_t i = 0; i < it->second.childCount; ++i)
        {
          _childToParent.erase(it->second.childIds[i]);
        }
        it = _parentStates.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  size_t size() const { return _parentStates.size(); }

  void clear()
  {
    _parentStates.clear();
    _childToParent.clear();
  }

 private:
  std::unordered_map<OrderId, SplitState> _parentStates;
  std::unordered_map<OrderId, OrderId> _childToParent;
};

}  // namespace flox
