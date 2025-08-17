/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>

namespace flox
{

using SubscriberId = uint64_t;

struct ISubscriber
{
  virtual SubscriberId id() const = 0;
};

}  // namespace flox