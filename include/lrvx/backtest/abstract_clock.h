/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/util/base/time.h"

namespace lrvx
{

class IClock
{
 public:
  virtual ~IClock() = default;

  virtual UnixNanos nowNs() const = 0;
  virtual void advanceTo(UnixNanos ns) = 0;
};

}  // namespace lrvx
