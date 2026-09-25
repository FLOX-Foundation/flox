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
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/abstract_logger.h>
#include <flox/net/abstract_transport.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace flox
{

template <typename Policies = NoPolicies>
class HyperliquidOrderExecutorT
{
 public:
  using RateLimitPolicy = typename Policies::RateLimitType;
  using TimeoutPolicy = typename Policies::TimeoutType;

  HyperliquidOrderExecutorT(std::string restUrl, std::string privateKeyHex,
                            SymbolRegistry* registry, OrderTracker* orderTracker,
                            std::shared_ptr<ILogger> logger, std::string accountAddress,
                            std::optional<std::string> vaultAddress, bool mainnet);

  // Transport-injecting overload. Production code always goes through the
  // constructor above, which owns a real CurlTransport; this one exists so
  // tests can hand in a fake ITransport and observe the exact request body
  // this executor builds, without a live signing helper or network access.
  HyperliquidOrderExecutorT(std::unique_ptr<ITransport> transport, std::string restUrl,
                            std::string privateKeyHex, SymbolRegistry* registry,
                            OrderTracker* orderTracker, std::shared_ptr<ILogger> logger,
                            std::string accountAddress, std::optional<std::string> vaultAddress,
                            bool mainnet);

  ~HyperliquidOrderExecutorT();

  template <typename P = Policies, typename = std::enable_if_t<P::RateLimitType::enabled>>
  HyperliquidOrderExecutorT(std::string restUrl, std::string privateKeyHex,
                            SymbolRegistry* registry, OrderTracker* orderTracker,
                            std::shared_ptr<ILogger> logger, std::string accountAddress,
                            std::optional<std::string> vaultAddress, bool mainnet,
                            RateLimitConfig rateLimitConfig);

  template <typename P = Policies, typename = std::enable_if_t<P::TimeoutType::enabled>>
  HyperliquidOrderExecutorT(std::string restUrl, std::string privateKeyHex,
                            SymbolRegistry* registry, OrderTracker* orderTracker,
                            std::shared_ptr<ILogger> logger, std::string accountAddress,
                            std::optional<std::string> vaultAddress, bool mainnet,
                            OrderTimeoutConfig timeoutConfig);

  template <typename P = Policies,
            typename = std::enable_if_t<P::RateLimitType::enabled && P::TimeoutType::enabled>>
  HyperliquidOrderExecutorT(std::string restUrl, std::string privateKeyHex,
                            SymbolRegistry* registry, OrderTracker* orderTracker,
                            std::shared_ptr<ILogger> logger, std::string accountAddress,
                            std::optional<std::string> vaultAddress, bool mainnet,
                            RateLimitConfig rateLimitConfig, OrderTimeoutConfig timeoutConfig);

  void submitOrder(const Order& order);
  void cancelOrder(OrderId id);
  void replaceOrder(OrderId oldId, const Order& n);

  // Without this the executor was mute: it held no bus and published no
  // OrderEvent on any path, so a fill Hyperliquid returned inline with the
  // submit reached no listener and a venue rejection was not even parsed --
  // the order was handed to the tracker as submitted and the strategy went on
  // believing a resting order existed. Same hook BybitOrderExecutorT has.
  void setOrderBus(OrderExecutionBus* bus) { _orderBus = bus; }

 private:
  void loadAssetIds();
  int assetIdFor(std::string_view coin);

  void publishRejection(const Order& order, const std::string& reason);
  void publishSubmitted(const Order& order);
  void publishFill(const Order& order, Quantity fillQty, Price fillPrice);

  std::string _url;
  std::string _privateKey;
  std::string _accountAddress;
  std::optional<std::string> _vaultAddress;
  bool _mainnet{true};

  SymbolRegistry* _registry;
  OrderTracker* _orderTracker;
  std::shared_ptr<ILogger> _logger;
  std::unique_ptr<ITransport> _transport;
  OrderExecutionBus* _orderBus = nullptr;

  // Thread-safe asset ID cache
  mutable std::mutex _assetMutex;
  std::unordered_map<std::string, int> _assetIds;
  bool _assetsLoaded{false};

  Policies _policies;
};

using HyperliquidOrderExecutor = HyperliquidOrderExecutorT<NoPolicies>;
using HyperliquidOrderExecutorWithRateLimit = HyperliquidOrderExecutorT<WithRateLimit>;
using HyperliquidOrderExecutorWithTimeout = HyperliquidOrderExecutorT<WithTimeout>;
using HyperliquidOrderExecutorFull = HyperliquidOrderExecutorT<FullPolicies>;

extern template class HyperliquidOrderExecutorT<NoPolicies>;
extern template class HyperliquidOrderExecutorT<WithRateLimit>;
extern template class HyperliquidOrderExecutorT<WithTimeout>;
extern template class HyperliquidOrderExecutorT<FullPolicies>;

}  // namespace flox
