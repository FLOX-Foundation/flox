/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/engine/abstract_subscriber.h"
#include "flox/execution/order.h"

namespace flox
{

class IOrderExecutionListener : public ISubscriber
{
  SubscriberId _id{};

 public:
  IOrderExecutionListener(SubscriberId id) : _id(id) {}

  virtual ~IOrderExecutionListener() = default;

  SubscriberId id() const override { return _id; };

  virtual void onOrderSubmitted(const Order& order) = 0;
  virtual void onOrderAccepted(const Order& order) = 0;
  virtual void onOrderPartiallyFilled(const Order& order, Quantity fillQty) = 0;
  virtual void onOrderFilled(const Order& order) = 0;
  virtual void onOrderCanceled(const Order& order) = 0;
  virtual void onOrderExpired(const Order& order) = 0;
  virtual void onOrderRejected(const Order& order, const std::string& reason) = 0;
  virtual void onOrderReplaced(const Order& oldOrder, const Order& newOrder) = 0;
};

}  // namespace flox