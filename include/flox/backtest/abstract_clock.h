/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/util/base/time.h"

namespace flox
{

class IClock
{
 public:
  virtual ~IClock() = default;

  virtual UnixNanos nowNs() const = 0;
  virtual void advanceTo(UnixNanos ns) = 0;
};

}  // namespace flox
