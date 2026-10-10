/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/backtest/abstract_clock.h"

namespace lrvx
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

  void reset(UnixNanos ns = UnixNanos{}) { _current_ns = ns; }

 private:
  UnixNanos _current_ns{0};
};

}  // namespace lrvx
