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

enum class OrderEventType
{
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
  OrderEventType type{};
  Order order{};
  Order newOrder{};
  Quantity fillQty{0};

  uint64_t tickSequence = 0;

  void dispatchTo(IOrderExecutionListener& listener) const
  {
    switch (type)
    {
      case OrderEventType::SUBMITTED:
        listener.onOrderSubmitted(order);
      case OrderEventType::ACCEPTED:
        listener.onOrderAccepted(order);
        break;
      case OrderEventType::PARTIALLY_FILLED:
        listener.onOrderPartiallyFilled(order, fillQty);
        break;
      case OrderEventType::FILLED:
        listener.onOrderFilled(order);
        break;
      case OrderEventType::CANCELED:
        listener.onOrderCanceled(order);
        break;
      case OrderEventType::EXPIRED:
        listener.onOrderExpired(order);
        break;
      case OrderEventType::REJECTED:
        listener.onOrderRejected(order, "");
        break;
      case OrderEventType::REPLACED:
        listener.onOrderReplaced(order, newOrder);
        break;
    }
  }
};

}  // namespace flox
