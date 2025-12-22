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
#include <flox/execution/order_tracker.h>
#include <flox/log/abstract_logger.h>
#include <flox/net/abstract_transport.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace flox
{

class HyperliquidOrderExecutor
{
 public:
  HyperliquidOrderExecutor(std::string restUrl, std::string privateKeyHex, SymbolRegistry* registry,
                           OrderTracker* orderTracker, std::shared_ptr<ILogger> logger,
                           std::string accountAddress, std::optional<std::string> vaultAddress,
                           bool mainnet);

  void submitOrder(const Order& order);
  void cancelOrder(OrderId id);
  void replaceOrder(OrderId oldId, const Order& n);

 private:
  void loadAssetIds();
  int assetIdFor(std::string_view coin);

  std::string _url;
  std::string _privateKey;
  std::string _accountAddress;
  std::optional<std::string> _vaultAddress;
  bool _mainnet{true};

  SymbolRegistry* _registry;
  OrderTracker* _orderTracker;
  std::shared_ptr<ILogger> _logger;
  std::unique_ptr<ITransport> _transport;

  // Thread-safe asset ID cache
  mutable std::mutex _assetMutex;
  std::unordered_map<std::string, int> _assetIds;
  bool _assetsLoaded{false};
};

}  // namespace flox
