/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

namespace flox
{

class ISubsystem
{
 public:
  virtual ~ISubsystem() = default;

  virtual void start() {};
  virtual void stop() {};
};

}  // namespace flox