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

#include <optional>

namespace flox
{

class IPositionManager : public ISubsystem, public IOrderExecutionListener
{
 public:
  IPositionManager(SubscriberId id) : IOrderExecutionListener(id) {}

  virtual ~IPositionManager() = default;

  virtual Quantity getPosition(SymbolId symbol) const = 0;

  // Average entry price of the open position, or nothing when this manager
  // keeps no cost basis. The strategy context reads it to work out unrealized
  // PnL. With no source for it the context reported position times mark --
  // the whole notional, not the profit on it -- so a rule like "close when the
  // loss passes X" never fired on a long and fired on the first tick of a
  // short.
  //
  // Defaulted to nothing so a custom manager written against the old
  // interface still compiles. The context then reports unrealized PnL as
  // unknown rather than inventing a number.
  virtual std::optional<Price> getAverageEntryPrice(SymbolId /*symbol*/) const
  {
    return std::nullopt;
  }
};

}  // namespace flox
