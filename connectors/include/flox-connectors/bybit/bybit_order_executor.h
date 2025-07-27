/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_executor.h>
#include <flox/execution/order.h>
#include <flox/execution/order_tracker.h>

#include <memory>

namespace flox
{

class AuthenticatedRestClient;

namespace Bybit
{

inline std::string_view toString(InstrumentType type)
{
  switch (type)
  {
    case InstrumentType::Spot:
      return "spot";
    case InstrumentType::Future:
      return "linear";
    case InstrumentType::Inverse:
      return "inverse";
    case InstrumentType::Option:
      return "option";
  }

  return "unknown";
}

}  // namespace Bybit

class BybitOrderExecutor : public IOrderExecutor
{
 public:
  explicit BybitOrderExecutor(std::unique_ptr<AuthenticatedRestClient> client, SymbolRegistry* registry, OrderTracker* orderTracker);
  ~BybitOrderExecutor() override;

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

 private:
  std::unique_ptr<AuthenticatedRestClient> _client;
  SymbolRegistry* _registry;
  OrderTracker* _orderTracker;
};

}  // namespace flox
