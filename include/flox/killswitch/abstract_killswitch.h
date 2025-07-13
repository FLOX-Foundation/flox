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

namespace flox
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

}  // namespace flox
