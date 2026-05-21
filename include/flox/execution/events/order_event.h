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
  QUEUE_POSITION_UPDATED
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

  // For fills and trailing updates
  Price fillPrice{};
  Price newTrailingPrice{};

  // For QUEUE_POSITION_UPDATED, PARTIALLY_FILLED, FILLED events on
  // backtest limit orders: volume ahead and total volume at the
  // order's level at the time the event was emitted. Zero on live
  // events and on non-limit orders.
  Quantity queueAhead{0};
  Quantity queueTotal{0};

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
    }
  }
};

}  // namespace flox
