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

// What a strategy needs from a position manager on a tick: the position and
// the cost basis it is measured against, together. Answering them separately
// means two questions about one piece of state, and on the shipped trackers
// that is two acquisitions of the same mutex and two walks over the same lot
// deque.
struct PositionSnapshot
{
  Quantity position{};
  std::optional<Price> avgEntryPrice;
};

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

  // Both of the above, from one question. Strategy::refreshPosition() runs on
  // every trade, every book update and every bar, and it used to ask the two
  // getters separately: on PositionTracker that is two acquisitions of the
  // tracker's mutex -- the one the execution thread also wants -- and two
  // passes over the symbol's lots, for a pair of values that come from the
  // same state under the same lock.
  //
  // The default answers with the position and no cost basis, deliberately
  // rather than by calling both getters: composing it here would reinstate
  // the second acquisition in the one place this exists to remove it from. A
  // manager that keeps a cost basis overrides this and fills in both from one
  // traversal; both shipped trackers do. That matches what the default
  // getAverageEntryPrice() above already says -- this manager keeps no cost
  // basis -- so a manager written against the older interface reports exactly
  // what it did before.
  //
  // Not named snapshot(): MultiModePositionTracker has had a snapshot() of
  // its own, per-side and with a different shape, since before this existed.
  virtual PositionSnapshot positionSnapshot(SymbolId symbol) const
  {
    return PositionSnapshot{getPosition(symbol), std::nullopt};
  }
};

}  // namespace flox
