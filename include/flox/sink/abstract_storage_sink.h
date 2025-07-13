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

class IStorageSink : public ISubsystem
{
 public:
  virtual ~IStorageSink() = default;

  virtual void store(const Order& order) = 0;
};

}  // namespace flox