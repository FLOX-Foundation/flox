/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/engine/abstract_market_data_subscriber.h"
#include "lrvx/engine/abstract_subsystem.h"

namespace lrvx
{

class ISignalHandler;
class IPositionManager;
struct OrderEvent;

class IStrategy : public ISubsystem, public IMarketDataSubscriber
{
 public:
  virtual ~IStrategy() = default;

  virtual void setSignalHandler(ISignalHandler*) {}
  virtual void setPositionManager(IPositionManager*) {}

  // Forwarded by the runner each time the executor publishes an
  // OrderEvent for an order this strategy emitted. Default no-op so
  // callers don't have to override; the concrete Strategy dispatches
  // to virtual `onSymbolFill` / `onSymbolOrderUpdate` based on
  // status.
  virtual void onOrderEvent(const OrderEvent&) {}
};

}  // namespace lrvx
