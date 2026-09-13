/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox-connectors/execution/executor_policies.h"

#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_executor.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/order.h>
#include <flox/execution/order_tracker.h>

#include <memory>
#include <string>

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

template <typename Policies = NoPolicies>
class BybitOrderExecutorT : public IOrderExecutor
{
 public:
  using RateLimitPolicy = typename Policies::RateLimitType;
  using TimeoutPolicy = typename Policies::TimeoutType;

  explicit BybitOrderExecutorT(std::unique_ptr<AuthenticatedRestClient> client,
                               SymbolRegistry* registry, OrderTracker* orderTracker)
      : _client(std::move(client)), _registry(registry), _orderTracker(orderTracker)
  {
  }

  template <typename P = Policies, typename = std::enable_if_t<P::RateLimitType::enabled>>
  explicit BybitOrderExecutorT(std::unique_ptr<AuthenticatedRestClient> client,
                               SymbolRegistry* registry, OrderTracker* orderTracker,
                               RateLimitConfig rateLimitConfig)
      : _client(std::move(client)), _registry(registry), _orderTracker(orderTracker)
  {
    _policies.rateLimit.init(std::move(rateLimitConfig));
  }

  template <typename P = Policies, typename = std::enable_if_t<P::TimeoutType::enabled>>
  explicit BybitOrderExecutorT(std::unique_ptr<AuthenticatedRestClient> client,
                               SymbolRegistry* registry, OrderTracker* orderTracker,
                               OrderTimeoutConfig timeoutConfig)
      : _client(std::move(client)), _registry(registry), _orderTracker(orderTracker)
  {
    _policies.timeout.init(std::move(timeoutConfig));
    _policies.timeout.start();
  }

  template <typename P = Policies,
            typename = std::enable_if_t<P::RateLimitType::enabled && P::TimeoutType::enabled>>
  explicit BybitOrderExecutorT(std::unique_ptr<AuthenticatedRestClient> client,
                               SymbolRegistry* registry, OrderTracker* orderTracker,
                               RateLimitConfig rateLimitConfig, OrderTimeoutConfig timeoutConfig)
      : _client(std::move(client)), _registry(registry), _orderTracker(orderTracker)
  {
    _policies.rateLimit.init(std::move(rateLimitConfig));
    _policies.timeout.init(std::move(timeoutConfig));
    _policies.timeout.start();
  }

  ~BybitOrderExecutorT() override;

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

  // Without this, a venue rejection was logged and dropped: no event on any
  // bus, no tracker record, and (since clearPending() ran before the
  // retCode check) the timeout watchdog disarmed itself at the exact moment
  // it should have fired. Bitget already has this; Bybit did not.
  void setOrderBus(OrderExecutionBus* bus) { _orderBus = bus; }

 private:
  void publishRejection(const Order& order, const std::string& reason);
  void publishRateLimited(const Order& order);

  std::unique_ptr<AuthenticatedRestClient> _client;
  SymbolRegistry* _registry;
  OrderTracker* _orderTracker;
  Policies _policies;
  OrderExecutionBus* _orderBus = nullptr;
};

using BybitOrderExecutor = BybitOrderExecutorT<NoPolicies>;
using BybitOrderExecutorWithRateLimit = BybitOrderExecutorT<WithRateLimit>;
using BybitOrderExecutorWithTimeout = BybitOrderExecutorT<WithTimeout>;
using BybitOrderExecutorFull = BybitOrderExecutorT<FullPolicies>;

extern template class BybitOrderExecutorT<NoPolicies>;
extern template class BybitOrderExecutorT<WithRateLimit>;
extern template class BybitOrderExecutorT<WithTimeout>;
extern template class BybitOrderExecutorT<FullPolicies>;

}  // namespace flox
