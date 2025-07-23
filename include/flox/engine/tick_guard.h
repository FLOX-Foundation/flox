/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/engine/tick_barrier.h"

namespace flox
{

class TickGuard
{
 public:
  explicit TickGuard(TickBarrier& barrier) : _barrier(&barrier) {}

  ~TickGuard()
  {
    if (_barrier)
    {
      _barrier->complete();
    }
  }

  TickGuard(const TickGuard&) = delete;
  TickGuard& operator=(const TickGuard&) = delete;

 private:
  TickBarrier* _barrier;
};

}  // namespace flox
