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

#include <chrono>

namespace flox
{

// Live (wall-clock) IClock implementation. Backtest drives time through
// SimulatedClock; live systems drive it through SystemClock, so time-dependent
// code (GTD expiry, last-look windows, funding intervals) can depend on the same
// IClock abstraction in both modes -- the production-parity promise. `nowNs`
// returns UNIX nanoseconds so it lines up with market-data timestamps.
class SystemClock : public IClock
{
 public:
  UnixNanos nowNs() const override
  {
    return static_cast<UnixNanos>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
  }

  // Real time cannot be advanced or rewound; the driver-side advance is a no-op
  // so a SystemClock is drop-in wherever an IClock is expected.
  void advanceTo(UnixNanos /*ns*/) override {}
};

}  // namespace flox
