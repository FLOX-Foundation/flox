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
#include <optional>
#include <vector>

namespace flox
{

enum class OrderGroupPolicy : uint8_t
{
  BestEffort = 0,    // submit every leg, observe independently
  AllOrNothing = 1,  // any leg failure → revert (close) every filled leg
  OneSided = 2,      // first leg fill → cancel remaining legs
};

enum class LegState : uint8_t
{
  Pending = 0,
  Submitted = 1,
  PartiallyFilled = 2,
  Filled = 3,
  Cancelled = 4,
  Failed = 5,
};

enum class OrderGroupState : uint8_t
{
  Pending = 0,           // not all legs submitted yet
  Submitted = 1,         // all legs submitted, none terminal
  PartiallyFilled = 2,   // some terminal (filled), not all
  Filled = 3,            // all legs filled
  Cancelled = 4,         // user-driven cancel ran
  Reverting = 5,         // AllOrNothing: a leg failed; recommend reverts
  Failed = 6,            // every leg in a non-fill terminal state
};

struct OrderGroupLeg
{
  SymbolId symbol{};
  uint8_t side = 0;        // 0 = BUY, 1 = SELL
  Quantity targetQty{};
  uint8_t orderType = 1;   // 0 = LIMIT, 1 = MARKET
  Price limitPrice{};      // valid when orderType == 0
  OrderId orderId = 0;
  Quantity filledQty{};
  LegState state = LegState::Pending;
};

// Recommended action surfaced by the state machine. The binding /
// strategy is responsible for issuing the action through its
// executor — `OrderGroup` itself does no I/O.
struct OrderGroupAction
{
  enum class Kind : uint8_t
  {
    CancelLeg = 0,   // cancel the existing order on this leg (OneSided)
    RevertLeg = 1,   // submit an opposite-side market order to undo a fill (AllOrNothing)
  };

  Kind kind{Kind::CancelLeg};
  size_t legIndex = 0;
  OrderId orderId = 0;       // for CancelLeg
  SymbolId symbol{};         // for RevertLeg
  uint8_t side = 0;          // for RevertLeg (opposite side of the original fill)
  Quantity qty{};            // for RevertLeg (filled qty to undo)
};

// `OrderGroup` is a passive state machine: callers feed it leg
// definitions and order/fill events; the group reports the aggregate
// state and the actions the strategy *should* run next. It owns no
// executor reference and performs no I/O.
class OrderGroup
{
 public:
  OrderGroup(uint64_t parentSignalId, OrderGroupPolicy policy)
      : _parentSignalId(parentSignalId), _policy(policy)
  {
  }

  uint64_t parentSignalId() const noexcept { return _parentSignalId; }
  OrderGroupPolicy policy() const noexcept { return _policy; }

  size_t addMarketLeg(SymbolId symbol, uint8_t side, Quantity qty)
  {
    OrderGroupLeg leg{};
    leg.symbol = symbol;
    leg.side = side;
    leg.targetQty = qty;
    leg.orderType = 1;
    _legs.push_back(leg);
    return _legs.size() - 1;
  }

  size_t addLimitLeg(SymbolId symbol, uint8_t side, Price price, Quantity qty)
  {
    OrderGroupLeg leg{};
    leg.symbol = symbol;
    leg.side = side;
    leg.targetQty = qty;
    leg.orderType = 0;
    leg.limitPrice = price;
    _legs.push_back(leg);
    return _legs.size() - 1;
  }

  size_t legCount() const noexcept { return _legs.size(); }

  const OrderGroupLeg& leg(size_t idx) const { return _legs.at(idx); }

  void recordSubmit(size_t legIdx, OrderId orderId)
  {
    auto& l = _legs.at(legIdx);
    l.orderId = orderId;
    l.state = LegState::Submitted;
  }

  // qty is the cumulative filled quantity on the leg, not a delta.
  void recordFill(size_t legIdx, Quantity cumulativeQty)
  {
    auto& l = _legs.at(legIdx);
    l.filledQty = cumulativeQty;
    if (l.filledQty.raw() >= l.targetQty.raw())
    {
      l.state = LegState::Filled;
    }
    else
    {
      l.state = LegState::PartiallyFilled;
    }
  }

  void recordCancel(size_t legIdx)
  {
    _legs.at(legIdx).state = LegState::Cancelled;
  }

  void recordFailure(size_t legIdx)
  {
    _legs.at(legIdx).state = LegState::Failed;
  }

  OrderGroupState state() const noexcept
  {
    if (_legs.empty())
    {
      return OrderGroupState::Pending;
    }
    bool anyPending = false;
    bool anyFilled = false;
    bool allFilled = true;
    bool anyFailed = false;
    bool anyCancelled = false;
    bool allTerminal = true;
    bool anyNonFilledTerminal = false;
    for (const auto& l : _legs)
    {
      switch (l.state)
      {
        case LegState::Pending:
          anyPending = true;
          allTerminal = false;
          allFilled = false;
          break;
        case LegState::Submitted:
        case LegState::PartiallyFilled:
          allTerminal = false;
          allFilled = false;
          break;
        case LegState::Filled:
          anyFilled = true;
          break;
        case LegState::Cancelled:
          anyCancelled = true;
          allFilled = false;
          anyNonFilledTerminal = true;
          break;
        case LegState::Failed:
          anyFailed = true;
          allFilled = false;
          anyNonFilledTerminal = true;
          break;
      }
    }

    if (anyPending)
    {
      return OrderGroupState::Pending;
    }
    if (allFilled)
    {
      return OrderGroupState::Filled;
    }
    if (_policy == OrderGroupPolicy::AllOrNothing && anyFailed)
    {
      return OrderGroupState::Reverting;
    }
    if (_policy == OrderGroupPolicy::OneSided && anyFilled && !allFilled)
    {
      return OrderGroupState::PartiallyFilled;
    }
    if (allTerminal && !anyFilled)
    {
      return anyCancelled ? OrderGroupState::Cancelled : OrderGroupState::Failed;
    }
    if (anyFilled || anyNonFilledTerminal)
    {
      return OrderGroupState::PartiallyFilled;
    }
    return OrderGroupState::Submitted;
  }

  // Recommended next actions given the current legs / policy. Empty
  // if nothing needs to happen.
  std::vector<OrderGroupAction> recommendedActions() const
  {
    std::vector<OrderGroupAction> out;
    if (_policy == OrderGroupPolicy::BestEffort)
    {
      return out;  // BestEffort never recommends side actions
    }

    if (_policy == OrderGroupPolicy::OneSided)
    {
      // First fill triggers cancellation of remaining open legs.
      bool anyFill = false;
      for (const auto& l : _legs)
      {
        if (l.state == LegState::Filled || l.state == LegState::PartiallyFilled)
        {
          anyFill = true;
          break;
        }
      }
      if (!anyFill)
      {
        return out;
      }
      for (size_t i = 0; i < _legs.size(); ++i)
      {
        const auto& l = _legs[i];
        if (l.state == LegState::Submitted || l.state == LegState::Pending)
        {
          OrderGroupAction a{};
          a.kind = OrderGroupAction::Kind::CancelLeg;
          a.legIndex = i;
          a.orderId = l.orderId;
          out.push_back(a);
        }
      }
      return out;
    }

    if (_policy == OrderGroupPolicy::AllOrNothing)
    {
      bool anyFailureOrCancel = false;
      for (const auto& l : _legs)
      {
        if (l.state == LegState::Failed || l.state == LegState::Cancelled)
        {
          anyFailureOrCancel = true;
          break;
        }
      }
      if (!anyFailureOrCancel)
      {
        return out;
      }
      // Cancel every still-open leg, revert every filled leg.
      for (size_t i = 0; i < _legs.size(); ++i)
      {
        const auto& l = _legs[i];
        if (l.state == LegState::Submitted || l.state == LegState::Pending)
        {
          OrderGroupAction a{};
          a.kind = OrderGroupAction::Kind::CancelLeg;
          a.legIndex = i;
          a.orderId = l.orderId;
          out.push_back(a);
        }
        else if (l.state == LegState::Filled || l.state == LegState::PartiallyFilled)
        {
          OrderGroupAction a{};
          a.kind = OrderGroupAction::Kind::RevertLeg;
          a.legIndex = i;
          a.symbol = l.symbol;
          a.side = (l.side == 0) ? 1 : 0;  // opposite side
          a.qty = l.filledQty;
          out.push_back(a);
        }
      }
      return out;
    }

    return out;
  }

 private:
  uint64_t _parentSignalId;
  OrderGroupPolicy _policy;
  std::vector<OrderGroupLeg> _legs;
};

}  // namespace flox
