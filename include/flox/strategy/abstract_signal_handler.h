/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/strategy/signal.h"

namespace flox
{

class ISignalHandler
{
 public:
  virtual ~ISignalHandler() = default;

  virtual void onSignal(const Signal& signal) = 0;
};

}  // namespace flox
