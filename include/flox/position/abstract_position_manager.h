/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"
#include "flox/engine/abstract_subscriber.h"
#include "flox/engine/abstract_subsystem.h"
#include "flox/execution/abstract_execution_listener.h"

namespace flox
{

class IPositionManager : public ISubsystem, public IOrderExecutionListener
{
 public:
  IPositionManager(SubscriberId id) : IOrderExecutionListener(id) {}

  virtual ~IPositionManager() = default;

  virtual Quantity getPosition(SymbolId symbol) const = 0;
};

}  // namespace flox
