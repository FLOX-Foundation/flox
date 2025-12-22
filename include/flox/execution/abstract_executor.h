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
#include "flox/execution/exchange_capabilities.h"
#include "flox/execution/order.h"

namespace flox
{

struct OCOParams
{
  Order order1;
  Order order2;
};

class IOrderExecutor : public ISubsystem
{
 public:
  virtual ~IOrderExecutor() = default;

  virtual void submitOrder(const Order& order) {}
  virtual void cancelOrder(OrderId orderId) {}
  virtual void cancelAllOrders(SymbolId symbol) {}
  virtual void replaceOrder(OrderId oldOrderId, const Order& newOrder) {}

  // OCO: one-cancels-other
  virtual void submitOCO(const OCOParams& params) {}

  // Capability discovery
  virtual ExchangeCapabilities capabilities() const { return ExchangeCapabilities::simulated(); }
};

}  // namespace flox
