/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/execution/abstract_execution_listener.h"
#include "flox/execution/order.h"

namespace flox
{

enum class OrderEventStatus
{
  NEW,
  SUBMITTED,
  ACCEPTED,
  PARTIALLY_FILLED,
  FILLED,
  PENDING_CANCEL,
  CANCELED,
  EXPIRED,
  REJECTED,
  REPLACED,
  // Conditional order statuses
  PENDING_TRIGGER,
  TRIGGERED,
  TRAILING_UPDATED,
  // Queue position changed without any other lifecycle transition.
  // Backtest-only: surfaces movement of `queueAhead` as proportional
  // shrink / queue-consumption fills happen at the order's level.
  QUEUE_POSITION_UPDATED,
  // Market position of a resting order moved across categorical
  // states (best, behind_best, mid_spread, level_empty, crossed).
  // Backtest-only.
  MARKET_POSITION_CHANGED,
  // Replace-in-flight states. SUBMITTED fires immediately on
  // replaceOrder(); ACCEPTED fires after the ack latency; REJECTED
  // fires when the original order has already filled or cannot be
  // replaced. REPLACED stays the terminal "old gone, new alive".
  REPLACE_SUBMITTED,
  REPLACE_ACCEPTED,
  REPLACE_REJECTED,
  // Rejected pre-submission by client-side rate-limit enforcement.
  // See RateLimitPolicy + SimulatedExecutor::setRateLimitPolicy.
  REJECTED_RATE_LIMIT,
  // On-chain (DEX) lifecycle. PENDING_ONCHAIN: broadcast to the mempool,
  // not yet confirmed (probabilistic). REVERTED: the chain rejected the
  // transaction (reason in rejectReason). REPLACED_GAS: re-broadcast with
  // higher gas, superseding the pending tx. Connector-driven; backtest
  // and CEX paths never emit these.
  PENDING_ONCHAIN,
  REVERTED,
  REPLACED_GAS
};

// Categorical position of a resting limit order relative to the
// current top-of-book on its side.
enum class MarketPosition : uint8_t
{
  Unknown = 0,
  Best,        // our level is best on our side
  BehindBest,  // there is a better level on our side
  MidSpread,   // our price is strictly between best bid and best ask
  LevelEmpty,  // only our orders remain at this level
  Crossed,     // our price crosses the opposite side
};

// Per-lifecycle-stage timestamps stamped by the engine when the
// corresponding status transition fires. Zero means "not reached
// yet". Engine-time on backtests, exchange-time on live.
struct OrderTimestamps
{
  int64_t submittedAtNs{0};
  int64_t acceptedAtNs{0};
  int64_t firstFillAtNs{0};
  int64_t lastFillAtNs{0};
  int64_t canceledAtNs{0};
  int64_t rejectedAtNs{0};
  int64_t triggeredAtNs{0};
  int64_t expiredAtNs{0};
};

struct OrderEvent
{
  using Listener = IOrderExecutionListener;
  OrderEventStatus status = OrderEventStatus::NEW;
  Order order{};
  Order newOrder{};
  Quantity fillQty{0};
  std::string rejectReason;

  // On-chain (DEX) metadata, filled by the connector. txHash identifies
  // the broadcast transaction; confirmations counts blocks since
  // inclusion. The REVERTED reason reuses rejectReason. Empty / zero on
  // CEX and backtest events.
  std::string txHash;
  uint32_t confirmations{0};

  // For fills and trailing updates
  Price fillPrice{};
  Price newTrailingPrice{};

  // For PARTIALLY_FILLED / FILLED: true if the order rested in the
  // book and was consumed by an aggressive opposite trade (maker);
  // false if the order arrived marketable and crossed the book
  // (taker). Meaningless for non-fill statuses.
  bool isMaker{false};

  // For QUEUE_POSITION_UPDATED, PARTIALLY_FILLED, FILLED events on
  // backtest limit orders: volume ahead and total volume at the
  // order's level at the time the event was emitted. Zero on live
  // events and on non-limit orders.
  Quantity queueAhead{0};
  Quantity queueTotal{0};

  // For MARKET_POSITION_CHANGED and every event on a resting limit
  // order: categorical position relative to top-of-book and signed
  // distance in ticks from best on our side. Positive = behind best,
  // negative = ahead of best (mid-spread / crossed).
  MarketPosition marketPosition{MarketPosition::Unknown};
  int32_t distanceToBestTicks{0};

  // Snapshot of per-lifecycle-stage timestamps for the order at the
  // moment this event was emitted. The slot corresponding to the
  // current status is the freshest; earlier stages carry their
  // historical timestamps. Unreached stages read zero.
  OrderTimestamps timestamps{};

  uint64_t tickSequence{0};  // internal, set by bus

  uint64_t recvNs{0};
  uint64_t publishNs{0};
  int64_t exchangeTsNs{0};

  void dispatchTo(IOrderExecutionListener& listener) const
  {
    switch (status)
    {
      case OrderEventStatus::NEW:
        break;
      case OrderEventStatus::SUBMITTED:
        listener.onOrderSubmitted(order);
        break;
      case OrderEventStatus::ACCEPTED:
        listener.onOrderAccepted(order);
        break;
      case OrderEventStatus::PARTIALLY_FILLED:
        listener.onOrderPartiallyFilled(order, fillQty);
        break;
      case OrderEventStatus::FILLED:
        listener.onOrderFilled(order);
        break;
      case OrderEventStatus::PENDING_CANCEL:
        listener.onOrderPendingCancel(order);
        break;
      case OrderEventStatus::CANCELED:
        listener.onOrderCanceled(order);
        break;
      case OrderEventStatus::EXPIRED:
        listener.onOrderExpired(order);
        break;
      case OrderEventStatus::REJECTED:
        listener.onOrderRejected(order, rejectReason);
        break;
      case OrderEventStatus::REPLACED:
        listener.onOrderReplaced(order, newOrder);
        break;
      case OrderEventStatus::PENDING_TRIGGER:
        listener.onOrderPendingTrigger(order);
        break;
      case OrderEventStatus::TRIGGERED:
        listener.onOrderTriggered(order);
        break;
      case OrderEventStatus::TRAILING_UPDATED:
        listener.onTrailingStopUpdated(order, newTrailingPrice);
        break;
      case OrderEventStatus::QUEUE_POSITION_UPDATED:
        listener.onOrderQueuePositionChange(order, queueAhead, queueTotal);
        break;
      case OrderEventStatus::MARKET_POSITION_CHANGED:
        listener.onOrderMarketPositionChange(
            order, static_cast<uint8_t>(marketPosition), distanceToBestTicks);
        break;
      case OrderEventStatus::REPLACE_SUBMITTED:
        listener.onOrderReplaceSubmitted(order, newOrder);
        break;
      case OrderEventStatus::REPLACE_ACCEPTED:
        listener.onOrderReplaceAccepted(order, newOrder);
        break;
      case OrderEventStatus::REPLACE_REJECTED:
        listener.onOrderReplaceRejected(order, newOrder, rejectReason);
        break;
      case OrderEventStatus::REJECTED_RATE_LIMIT:
        listener.onOrderRejected(order, rejectReason);
        break;
      case OrderEventStatus::PENDING_ONCHAIN:
        listener.onOrderPendingOnchain(order, txHash);
        break;
      case OrderEventStatus::REVERTED:
        listener.onOrderReverted(order, rejectReason);
        break;
      case OrderEventStatus::REPLACED_GAS:
        listener.onOrderGasReplaced(order, newOrder);
        break;
    }
  }
};

}  // namespace flox
