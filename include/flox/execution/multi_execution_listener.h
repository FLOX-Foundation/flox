/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/engine/abstract_subscriber.h"
#include "flox/execution/abstract_execution_listener.h"

#include <algorithm>
#include <vector>

namespace flox
{

class MultiExecutionListener : public IOrderExecutionListener
{
 public:
  MultiExecutionListener(SubscriberId id) : IOrderExecutionListener(id) {}

  void addListener(IOrderExecutionListener* listener)
  {
    if (listener && std::ranges::find(_listeners, listener) == _listeners.end())
    {
      _listeners.push_back(listener);
    }
  }

  void onOrderSubmitted(const Order& order) override
  {
    std::ranges::for_each(_listeners,
                          [&](auto* l)
                          { l->onOrderSubmitted(order); });
  }

  void onOrderAccepted(const Order& order) override
  {
    std::ranges::for_each(_listeners,
                          [&](auto* l)
                          { l->onOrderAccepted(order); });
  }

  void onOrderPartiallyFilled(const Order& order, Quantity fillQty) override
  {
    std::ranges::for_each(
        _listeners,
        [&](auto* l)
        { l->onOrderPartiallyFilled(order, fillQty); });
  }

  void onOrderFilled(const Order& order) override
  {
    std::ranges::for_each(_listeners,
                          [&](auto* l)
                          { l->onOrderFilled(order); });
  }

  void onOrderPendingCancel(const Order& order) override
  {
    std::ranges::for_each(_listeners,
                          [&](auto* l)
                          { l->onOrderPendingCancel(order); });
  }

  void onOrderCanceled(const Order& order) override
  {
    std::ranges::for_each(_listeners,
                          [&](auto* l)
                          { l->onOrderCanceled(order); });
  }

  void onOrderExpired(const Order& order) override
  {
    std::ranges::for_each(_listeners,
                          [&](auto* l)
                          { l->onOrderExpired(order); });
  }

  void onOrderRejected(const Order& order, const std::string& reason) override
  {
    std::ranges::for_each(_listeners,
                          [&](auto* l)
                          { l->onOrderRejected(order, reason); });
  }

  void onOrderReplaced(const Order& oldOrder, const Order& newOrder) override
  {
    std::ranges::for_each(
        _listeners,
        [&](auto* l)
        { l->onOrderReplaced(oldOrder, newOrder); });
  }

  // Conditional-order callbacks.
  void onOrderPendingTrigger(const Order& order) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderPendingTrigger(order); });
  }

  void onOrderTriggered(const Order& order) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderTriggered(order); });
  }

  void onTrailingStopUpdated(const Order& order, Price newTriggerPrice) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onTrailingStopUpdated(order, newTriggerPrice); });
  }

  // Queue / market-position transitions.
  void onOrderQueuePositionChange(const Order& order, Quantity queueAhead,
                                  Quantity queueTotal) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderQueuePositionChange(order, queueAhead, queueTotal); });
  }

  void onOrderMarketPositionChange(const Order& order, uint8_t position,
                                   int32_t distanceToBestTicks) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderMarketPositionChange(order, position, distanceToBestTicks); });
  }

  // Replace-in-flight lifecycle.
  void onOrderReplaceSubmitted(const Order& oldOrder, const Order& newOrder) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderReplaceSubmitted(oldOrder, newOrder); });
  }

  void onOrderReplaceAccepted(const Order& oldOrder, const Order& newOrder) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderReplaceAccepted(oldOrder, newOrder); });
  }

  void onOrderReplaceRejected(const Order& oldOrder, const Order& newOrder,
                              const std::string& reason) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderReplaceRejected(oldOrder, newOrder, reason); });
  }

  // On-chain (DEX) lifecycle.
  void onOrderPendingOnchain(const Order& order, const std::string& txHash) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderPendingOnchain(order, txHash); });
  }

  void onOrderReverted(const Order& order, const std::string& reason) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderReverted(order, reason); });
  }

  void onOrderGasReplaced(const Order& oldOrder, const Order& newOrder) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderGasReplaced(oldOrder, newOrder); });
  }

  // Raw OrderEvent fan-out — the hook OrderJourneyTracer relies on.
  void onOrderEvent(const OrderEvent& ev) override
  {
    std::ranges::for_each(_listeners, [&](auto* l)
                          { l->onOrderEvent(ev); });
  }

 private:
  std::vector<IOrderExecutionListener*> _listeners;
};

}  // namespace flox
