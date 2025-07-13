/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/engine/abstract_subsystem.h"

namespace flox
{

class IStrategy : public ISubsystem, public IMarketDataSubscriber
{
 public:
  virtual ~IStrategy() = default;
};

}  // namespace flox
