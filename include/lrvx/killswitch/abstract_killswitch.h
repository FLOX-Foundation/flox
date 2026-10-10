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

class IKillSwitch : public ISubsystem
{
 public:
  virtual ~IKillSwitch() = default;

  virtual void check(const Order& order) = 0;
  virtual void trigger(const std::string& reason) = 0;
  virtual bool isTriggered() const = 0;
  virtual std::string reason() const = 0;
};

}  // namespace lrvx
