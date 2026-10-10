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

namespace lrvx
{

class IPnLTracker : public ISubsystem
{
 public:
  virtual ~IPnLTracker() = default;

  virtual void onOrderFilled(const Order& order) = 0;
};

}  // namespace lrvx
