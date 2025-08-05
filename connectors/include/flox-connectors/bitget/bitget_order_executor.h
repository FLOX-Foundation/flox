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

class BitgetOrderExecutor : public IOrderExecutor
{
 public:
  BitgetOrderExecutor(std::unique_ptr<BitgetAuthenticatedRestClient> client,
                      SymbolRegistry* registry, OrderTracker* orderTracker, Bitget::Params params);
  ~BitgetOrderExecutor() override;

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

 private:
  Bitget::Params _params;
  std::unique_ptr<BitgetAuthenticatedRestClient> _client;
  SymbolRegistry* _registry;
  OrderTracker* _orderTracker;
};

}  // namespace flox
