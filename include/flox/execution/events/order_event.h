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
  CANCELED,
  EXPIRED,
  REJECTED,
  REPLACED
};

struct OrderEvent
{
  using Listener = IOrderExecutionListener;
  OrderEventStatus status = OrderEventStatus::NEW;
  Order order{};
  Order newOrder{};
  Quantity fillQty{0};

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
      case OrderEventStatus::CANCELED:
        listener.onOrderCanceled(order);
        break;
      case OrderEventStatus::EXPIRED:
        listener.onOrderExpired(order);
        break;
      case OrderEventStatus::REJECTED:
        listener.onOrderRejected(order, "");
        break;
      case OrderEventStatus::REPLACED:
        listener.onOrderReplaced(order, newOrder);
        break;
    }
  }
};

}  // namespace flox
