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
#include <flox/execution/order.h>
#include <flox/execution/order_tracker.h>

#include <memory>
#include <string_view>

namespace flox
{

class BitgetAuthenticatedRestClient;

namespace Bitget
{

inline std::string_view category(InstrumentType type)
{
  switch (type)
  {
    case InstrumentType::Spot:
      return "spot";
    case InstrumentType::Future:
    case InstrumentType::Inverse:
      return "mix";
    case InstrumentType::Option:
      return "option";
  }
  return "mix";
}

struct Params
{
  std::string productType;
  std::string marginCoin;
  std::string marginMode;
  std::string forcePolicy;
};

}  // namespace Bitget

template <typename Policies = NoPolicies>
class BitgetOrderExecutorT : public IOrderExecutor
{
 public:
  using RateLimitPolicy = typename Policies::RateLimitType;
  using TimeoutPolicy = typename Policies::TimeoutType;

  explicit BitgetOrderExecutorT(std::unique_ptr<BitgetAuthenticatedRestClient> client,
                                SymbolRegistry* registry, OrderTracker* orderTracker,
                                Bitget::Params params)
      : _client(std::move(client)),
        _registry(registry),
        _orderTracker(orderTracker),
        _params(std::move(params))
  {
  }

  template <typename P = Policies, typename = std::enable_if_t<P::RateLimitType::enabled>>
  explicit BitgetOrderExecutorT(std::unique_ptr<BitgetAuthenticatedRestClient> client,
                                SymbolRegistry* registry, OrderTracker* orderTracker,
                                Bitget::Params params, RateLimitConfig rateLimitConfig)
      : _client(std::move(client)),
        _registry(registry),
        _orderTracker(orderTracker),
        _params(std::move(params))
  {
    _policies.rateLimit.init(std::move(rateLimitConfig));
  }

  template <typename P = Policies, typename = std::enable_if_t<P::TimeoutType::enabled>>
  explicit BitgetOrderExecutorT(std::unique_ptr<BitgetAuthenticatedRestClient> client,
                                SymbolRegistry* registry, OrderTracker* orderTracker,
                                Bitget::Params params, OrderTimeoutConfig timeoutConfig)
      : _client(std::move(client)),
        _registry(registry),
        _orderTracker(orderTracker),
        _params(std::move(params))
  {
    _policies.timeout.init(std::move(timeoutConfig));
    _policies.timeout.start();
  }

  template <typename P = Policies,
            typename = std::enable_if_t<P::RateLimitType::enabled && P::TimeoutType::enabled>>
  explicit BitgetOrderExecutorT(std::unique_ptr<BitgetAuthenticatedRestClient> client,
                                SymbolRegistry* registry, OrderTracker* orderTracker,
                                Bitget::Params params, RateLimitConfig rateLimitConfig,
                                OrderTimeoutConfig timeoutConfig)
      : _client(std::move(client)),
        _registry(registry),
        _orderTracker(orderTracker),
        _params(std::move(params))
  {
    _policies.rateLimit.init(std::move(rateLimitConfig));
    _policies.timeout.init(std::move(timeoutConfig));
    _policies.timeout.start();
  }

  ~BitgetOrderExecutorT() override;

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

 private:
  std::unique_ptr<BitgetAuthenticatedRestClient> _client;
  SymbolRegistry* _registry;
  OrderTracker* _orderTracker;
  Bitget::Params _params;
  Policies _policies;
};

using BitgetOrderExecutor = BitgetOrderExecutorT<NoPolicies>;
using BitgetOrderExecutorWithRateLimit = BitgetOrderExecutorT<WithRateLimit>;
using BitgetOrderExecutorWithTimeout = BitgetOrderExecutorT<WithTimeout>;
using BitgetOrderExecutorFull = BitgetOrderExecutorT<FullPolicies>;

extern template class BitgetOrderExecutorT<NoPolicies>;
extern template class BitgetOrderExecutorT<WithRateLimit>;
extern template class BitgetOrderExecutorT<WithTimeout>;
extern template class BitgetOrderExecutorT<FullPolicies>;

}  // namespace flox
