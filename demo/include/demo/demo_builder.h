/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/engine/engine.h"
#include "lrvx/engine/engine_config.h"

#include <memory>

namespace demo
{
using namespace lrvx;

class DemoBuilder
{
 public:
  explicit DemoBuilder(const EngineConfig& cfg);
  std::unique_ptr<Engine> build();

 private:
  EngineConfig _config;
};

}  // namespace demo
