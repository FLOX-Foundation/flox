/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>

namespace lrvx
{

using SubscriberId = uint64_t;

struct ISubscriber
{
  virtual SubscriberId id() const = 0;
};

}  // namespace lrvx