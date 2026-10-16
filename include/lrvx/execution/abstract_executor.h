/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/engine/abstract_subsystem.h"
#include "lrvx/execution/exchange_capabilities.h"
#include "lrvx/execution/order.h"

#include <span>

namespace lrvx
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

  // Batch submit. The default walks the batch through submitOrder, so every
  // executor accepts batches and single-order semantics are preserved; an
  // adapter for a venue with a real batch endpoint (OKX
  // /api/v5/trade/batch-orders, Binance /fapi/v1/batchOrders) overrides this
  // to coalesce the batch into one request — and advertises it via
  // capabilities().supportsBatchSubmit. A two-sided quoter that emits its
  // bid and ask together then pays one round trip per requote instead of
  // two. A venue cap on orders-per-request is the override's problem:
  // callers hand over the whole batch, the adapter chunks.
  virtual void submitOrders(std::span<const Order> orders)
  {
    for (const auto& order : orders)
    {
      submitOrder(order);
    }
  }

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

}  // namespace lrvx
