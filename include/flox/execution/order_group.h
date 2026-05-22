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
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flox
{

enum class OrderGroupPolicy : uint8_t
{
  BestEffort = 0,    // submit every leg, observe independently
  AllOrNothing = 1,  // any leg failure → revert (close) every filled leg
  OneSided = 2,      // first leg fill → cancel remaining legs
};

// Per-group risk limits checked before any leg is submitted. Each
// field is a hard cap; zero means "no limit on this dimension".
//
// `maxGrossNotional` is the absolute notional sum across legs
// (price * qty). `maxConcentrationPct` is the basket gross notional
// expressed as a fraction of the equity passed into the precheck —
// e.g. 0.05 means "no leg basket bigger than 5% of equity". Legs
// with `orderType == 1` (MARKET) need a reference price supplied
// by the caller; the precheck call accepts a per-leg fallback price
// vector for that.
struct GroupRiskLimits
{
  Quantity maxGrossNotional{};    // absolute notional cap (zero = off)
  double maxConcentrationPct{0};  // fraction of equity (zero = off)
  Quantity maxLegQty{};           // per-leg qty cap (zero = off)
};

struct GroupRiskBreach
{
  bool denied = false;
  std::string rule;
  std::string detail;
};

// Decision returned by `pairLatencyDecision`: should the strategy
// submit the follower leg (leader acked in time), cancel the leader
// leg (leader timed out), or wait (decision is still pending —
// neither submitted nor acked yet)?
enum class PairLatencyDecision : uint8_t
{
  Wait = 0,            // budget not yet evaluated / no decision needed
  SubmitFollower = 1,  // leader acked within budget — go ahead with leg B
  CancelLeader = 2,    // leader timed out — pull the open leg, abort basket
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
  Pending = 0,          // not all legs submitted yet
  Submitted = 1,        // all legs submitted, none terminal
  PartiallyFilled = 2,  // some terminal (filled), not all
  Filled = 3,           // all legs filled
  Cancelled = 4,        // user-driven cancel ran
  Reverting = 5,        // AllOrNothing: a leg failed; recommend reverts
  Failed = 6,           // every leg in a non-fill terminal state
};

struct OrderGroupLeg
{
  SymbolId symbol{};
  uint8_t side = 0;  // 0 = BUY, 1 = SELL
  Quantity targetQty{};
  uint8_t orderType = 1;  // 0 = LIMIT, 1 = MARKET
  Price limitPrice{};     // valid when orderType == 0
  OrderId orderId = 0;
  Quantity filledQty{};
  LegState state = LegState::Pending;
  // Bitmask of actions already dispatched to the executor:
  //   bit 0 = CancelLeg, bit 1 = RevertLeg.
  // The auto-dispatch helper sets these so a leg's cancel /
  // revert is fired exactly once even if `recommendedActions()` is
  // queried repeatedly.
  uint8_t dispatchedActions = 0;
};

// Recommended action surfaced by the state machine. The binding /
// strategy is responsible for issuing the action through its
// executor — `OrderGroup` itself does no I/O.
struct OrderGroupAction
{
  enum class Kind : uint8_t
  {
    CancelLeg = 0,  // cancel the existing order on this leg (OneSided)
    RevertLeg = 1,  // submit an opposite-side market order to undo a fill (AllOrNothing)
  };

  Kind kind{Kind::CancelLeg};
  size_t legIndex = 0;
  OrderId orderId = 0;  // for CancelLeg
  SymbolId symbol{};    // for RevertLeg
  uint8_t side = 0;     // for RevertLeg (opposite side of the original fill)
  Quantity qty{};       // for RevertLeg (filled qty to undo)
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

  // Risk limits applied across the basket. Zeroed by default — call
  // setRiskLimits to enable any of the three caps.
  void setRiskLimits(const GroupRiskLimits& limits) noexcept { _limits = limits; }
  const GroupRiskLimits& riskLimits() const noexcept { return _limits; }

  // OneSided pair latency budget. After the leader leg submits, the
  // strategy waits up to `budgetNs` for the leader's exchange ack
  // before sending the follower leg. If the ack lands within budget
  // the call to `pairLatencyDecision` returns SubmitFollower; if the
  // ack timestamp exceeds the budget (or the strategy passes a "now"
  // past the budget without an ack), CancelLeader.
  //
  // Use feed-time timestamps (deterministic under replay), not wall
  // clock. Zero `budgetNs` disables the gate.
  void setPairLatencyBudgetNs(int64_t budgetNs) noexcept { _pairBudgetNs = budgetNs; }
  int64_t pairLatencyBudgetNs() const noexcept { return _pairBudgetNs; }

  // Returns the decision the strategy should act on. Pass the leader's
  // submit timestamp and either the ack timestamp (if it has landed)
  // or the current feed time (treated as "still no ack"). The leg
  // index is the leader leg — typically 0 for OneSided pairs.
  PairLatencyDecision pairLatencyDecision(int64_t leaderSubmitTsNs,
                                          int64_t leaderAckTsNs,
                                          bool ackReceived) const noexcept
  {
    if (_pairBudgetNs <= 0)
    {
      return PairLatencyDecision::Wait;
    }
    if (ackReceived)
    {
      const int64_t latency = leaderAckTsNs - leaderSubmitTsNs;
      if (latency <= _pairBudgetNs)
      {
        return PairLatencyDecision::SubmitFollower;
      }
      return PairLatencyDecision::CancelLeader;
    }
    // No ack yet — caller passed current feed-time as `leaderAckTsNs`.
    const int64_t elapsed = leaderAckTsNs - leaderSubmitTsNs;
    if (elapsed > _pairBudgetNs)
    {
      return PairLatencyDecision::CancelLeader;
    }
    return PairLatencyDecision::Wait;
  }

  // Run the configured limits against the current legs. Each leg
  // contributes `referencePrice * targetQty` to gross notional;
  // for limit legs the leg's stored limitPrice is used, for market
  // legs the caller supplies a reference price via marketRefPrices
  // (one entry per leg in addMarketLeg order; missing entries are
  // treated as zero-priced and skip the gross check).
  // `equity` is the strategy's current equity; only consulted when
  // maxConcentrationPct > 0. Returns a breach record; .denied=false
  // means submit is allowed.
  GroupRiskBreach precheckSubmission(double equity = 0.0,
                                     const std::vector<Price>& marketRefPrices = {}) const
  {
    GroupRiskBreach out;
    if (_limits.maxGrossNotional.raw() == 0 && _limits.maxConcentrationPct == 0 &&
        _limits.maxLegQty.raw() == 0)
    {
      return out;
    }

    double grossNotional = 0.0;
    for (size_t i = 0; i < _legs.size(); ++i)
    {
      const auto& l = _legs[i];

      if (_limits.maxLegQty.raw() != 0 && l.targetQty.raw() > _limits.maxLegQty.raw())
      {
        out.denied = true;
        out.rule = "maxLegQty";
        out.detail = "leg " + std::to_string(i) + " qty exceeds per-leg cap";
        return out;
      }

      double price = 0.0;
      if (l.orderType == 0)
      {
        price = l.limitPrice.toDouble();
      }
      else if (i < marketRefPrices.size())
      {
        price = marketRefPrices[i].toDouble();
      }
      grossNotional += std::abs(price * l.targetQty.toDouble());
    }

    if (_limits.maxGrossNotional.raw() != 0 &&
        grossNotional > _limits.maxGrossNotional.toDouble())
    {
      out.denied = true;
      out.rule = "maxGrossNotional";
      out.detail = "basket gross notional exceeds cap";
      return out;
    }

    if (_limits.maxConcentrationPct > 0 && equity > 0)
    {
      double frac = grossNotional / equity;
      if (frac > _limits.maxConcentrationPct)
      {
        out.denied = true;
        out.rule = "maxConcentrationPct";
        out.detail =
            "basket gross notional exceeds concentration limit "
            "vs equity";
        return out;
      }
    }
    return out;
  }

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

  // Replace-ack landed. Swap in the new exchange order id; the leg
  // keeps its current LegState (Submitted / PartiallyFilled) so the
  // policy state machine continues to track the same leg slot.
  void recordReplaceAccepted(size_t legIdx, OrderId newOrderId)
  {
    auto& l = _legs.at(legIdx);
    if (l.state == LegState::Submitted || l.state == LegState::PartiallyFilled)
    {
      l.orderId = newOrderId;
    }
  }

  // Replace was rejected (late-replace race, post-only crossed, etc.).
  // The existing order on the leg stays live; no state change. Kept
  // as a no-op for API symmetry and as a hook for future telemetry.
  void recordReplaceRejected(size_t /*legIdx*/) noexcept {}

  // Map an exchange order id back to a leg slot. Returns nullopt if
  // no leg owns this id. Used by strategies routing an executor
  // replace event back to the OrderGroup.
  std::optional<size_t> findLegByOrderId(OrderId orderId) const noexcept
  {
    for (size_t i = 0; i < _legs.size(); ++i)
    {
      if (_legs[i].orderId == orderId)
      {
        return i;
      }
    }
    return std::nullopt;
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

  // Mark a leg's action as dispatched so it is not surfaced again
  // by recommendedActions(). Called by the binding-side auto-dispatch
  // helper after the action has been emitted through the executor.
  void markActionDispatched(size_t legIdx, OrderGroupAction::Kind kind)
  {
    auto& l = _legs.at(legIdx);
    uint8_t bit = (kind == OrderGroupAction::Kind::CancelLeg) ? 0x1 : 0x2;
    l.dispatchedActions |= bit;
  }

  // Recommended next actions given the current legs / policy. Empty
  // if nothing needs to happen. Actions already marked dispatched
  // (via `markActionDispatched`) are filtered out.
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
        if ((l.state == LegState::Submitted || l.state == LegState::Pending) &&
            !(l.dispatchedActions & 0x1))
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
        if ((l.state == LegState::Submitted || l.state == LegState::Pending) &&
            !(l.dispatchedActions & 0x1))
        {
          OrderGroupAction a{};
          a.kind = OrderGroupAction::Kind::CancelLeg;
          a.legIndex = i;
          a.orderId = l.orderId;
          out.push_back(a);
        }
        else if ((l.state == LegState::Filled || l.state == LegState::PartiallyFilled) &&
                 !(l.dispatchedActions & 0x2))
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
  GroupRiskLimits _limits{};
  int64_t _pairBudgetNs = 0;
};

}  // namespace flox
