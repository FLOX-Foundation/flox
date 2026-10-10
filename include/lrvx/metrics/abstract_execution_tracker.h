/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/engine/abstract_subsystem.h"
#include "lrvx/execution/order.h"

#include <chrono>
#include <string>

namespace lrvx
{

class IExecutionTracker : public ISubsystem
{
 public:
  virtual ~IExecutionTracker() = default;

  virtual void onOrderSubmitted(const Order& order,
                                TimePoint ts) = 0;

  virtual void onOrderAccepted(const Order& order,
                               TimePoint ts) = 0;

  virtual void onOrderPartiallyFilled(const Order& order, Quantity fillQty,
                                      TimePoint ts) = 0;

  virtual void onOrderFilled(const Order& order,
                             TimePoint ts) = 0;

  virtual void onOrderCanceled(const Order& order,
                               TimePoint ts) = 0;

  virtual void onOrderExpired(const Order& order,
                              TimePoint ts) = 0;

  virtual void onOrderRejected(const Order& order, const std::string& reason,
                               TimePoint ts) = 0;

  virtual void onOrderReplaced(const Order& oldOrder, const Order& newOrder,
                               TimePoint ts) = 0;
};

}  // namespace lrvx
