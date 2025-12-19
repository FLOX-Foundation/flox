/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/abstract_clock.h"

namespace flox
{

class SimulatedClock : public IClock
{
 public:
  SimulatedClock() = default;
  explicit SimulatedClock(UnixNanos initial) : _current_ns(initial) {}

  UnixNanos nowNs() const override { return _current_ns; }

  void advanceTo(UnixNanos ns) override
  {
    if (ns > _current_ns)
    {
      _current_ns = ns;
    }
  }

  void reset(UnixNanos ns = 0) { _current_ns = ns; }

 private:
  UnixNanos _current_ns{0};
};

}  // namespace flox
