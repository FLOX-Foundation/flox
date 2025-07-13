/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/engine/abstract_subsystem.h"
#include "flox/execution/order.h"

#include <chrono>
#include <string>

namespace flox
{

class IExecutionTracker : public ISubsystem
{
 public:
  virtual ~IExecutionTracker() = default;

  virtual void onOrderSubmitted(const Order& order,
                                std::chrono::steady_clock::time_point ts) = 0;

  virtual void onOrderAccepted(const Order& order,
                               std::chrono::steady_clock::time_point ts) = 0;

  virtual void onOrderPartiallyFilled(const Order& order, Quantity fillQty,
                                      std::chrono::steady_clock::time_point ts) = 0;

  virtual void onOrderFilled(const Order& order,
                             std::chrono::steady_clock::time_point ts) = 0;

  virtual void onOrderCanceled(const Order& order,
                               std::chrono::steady_clock::time_point ts) = 0;

  virtual void onOrderExpired(const Order& order,
                              std::chrono::steady_clock::time_point ts) = 0;

  virtual void onOrderRejected(const Order& order, const std::string& reason,
                               std::chrono::steady_clock::time_point ts) = 0;

  virtual void onOrderReplaced(const Order& oldOrder, const Order& newOrder,
                               std::chrono::steady_clock::time_point ts) = 0;
};

}  // namespace flox
