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

 private:
  std::vector<IOrderExecutionListener*> _listeners;
};

}  // namespace flox
