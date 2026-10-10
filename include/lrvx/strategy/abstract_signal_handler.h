/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/strategy/signal.h"

namespace lrvx
{

class ISignalHandler
{
 public:
  virtual ~ISignalHandler() = default;

  virtual void onSignal(const Signal& signal) = 0;
};

}  // namespace lrvx
