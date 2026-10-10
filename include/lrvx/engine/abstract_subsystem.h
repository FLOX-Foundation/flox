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

class ISubsystem
{
 public:
  virtual ~ISubsystem() = default;

  virtual void start() {}
  virtual void stop() {}
};

class IDrainable
{
 public:
  virtual ~IDrainable() = default;
  virtual bool drain(uint32_t timeoutMs) = 0;
};

}  // namespace lrvx