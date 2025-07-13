/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/engine/abstract_subsystem.h"
#include "flox/execution/order.h"

namespace flox
{

class IOrderExecutor : public ISubsystem
{
 public:
  virtual ~IOrderExecutor() = default;

  virtual void submitOrder(const Order& order) {};
  virtual void cancelOrder(OrderId orderId) {};
  virtual void replaceOrder(OrderId oldOrderId, const Order& newOrder) {};
};

}  // namespace flox
