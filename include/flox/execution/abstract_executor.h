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

  // Pure virtual: every executor must submit. A no-op default here silently
  // swallowed orders from any subclass that forgot to override it.
  virtual void submitOrder(const Order& order) = 0;

  // Lifecycle methods default to no-op. An executor that leaves one unoverridden
  // must NOT advertise the matching capability (see capabilities()): OCO relies
  // on cancelOrder, cancelAllOrders on a real cancel path, and so on. The
  // capability layer is what tells callers which of these actually do something.
  virtual void cancelOrder(OrderId orderId) {}
  virtual void cancelAllOrders(SymbolId symbol) {}
  virtual void replaceOrder(OrderId oldOrderId, const Order& newOrder) {}
  virtual void submitOCO(const OCOParams& params) {}

  // Capability discovery. The base advertises NOTHING: a real venue adapter must
  // declare what it supports, so a forgotten override cannot masquerade as full
  // support. The simulator overrides this with the permissive set it genuinely
  // implements.
  virtual ExchangeCapabilities capabilities() const { return ExchangeCapabilities::none(); }
};

}  // namespace flox
