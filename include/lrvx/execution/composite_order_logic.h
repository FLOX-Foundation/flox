/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/execution/abstract_execution_listener.h"
#include "lrvx/execution/abstract_executor.h"

#include <unordered_map>

namespace lrvx
{

class CompositeOrderLogic : public IOrderExecutionListener
{
 public:
  explicit CompositeOrderLogic(SubscriberId id) : IOrderExecutionListener(id) {}

  void setExecutor(IOrderExecutor* executor) noexcept { _executor = executor; }

  // Register OCO: when one fills/cancels, cancel the other
  void registerOCO(OrderId id1, OrderId id2)
  {
    _ocoLinks[id1] = id2;
    _ocoLinks[id2] = id1;
  }

  void onOrderFilled(const Order& order) override { handleOCO(order.id); }

  void onOrderCanceled(const Order& order) override { handleOCO(order.id); }

 private:
  void handleOCO(OrderId orderId)
  {
    auto ocoIt = _ocoLinks.find(orderId);
    if (ocoIt != _ocoLinks.end())
    {
      OrderId linkedId = ocoIt->second;

      // Remove both links before canceling to avoid recursion
      _ocoLinks.erase(orderId);
      _ocoLinks.erase(linkedId);

      if (_executor)
      {
        _executor->cancelOrder(linkedId);
      }
    }
  }

  IOrderExecutor* _executor{nullptr};
  std::unordered_map<OrderId, OrderId> _ocoLinks;
};

}  // namespace lrvx
