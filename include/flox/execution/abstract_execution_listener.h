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

  virtual void onOrderSubmitted(const Order&) {}
  virtual void onOrderAccepted(const Order&) {}
  virtual void onOrderPartiallyFilled(const Order&, Quantity) {}
  virtual void onOrderFilled(const Order&) {}
  virtual void onOrderPendingCancel(const Order&) {}
  virtual void onOrderCanceled(const Order&) {}
  virtual void onOrderExpired(const Order&) {}
  virtual void onOrderRejected(const Order&, const std::string&) {}
  virtual void onOrderReplaced(const Order&, const Order&) {}
};

}  // namespace flox