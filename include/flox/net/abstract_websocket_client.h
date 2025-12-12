/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <string>
#include <string_view>

#include "flox/engine/abstract_subsystem.h"
#include "flox/util/base/move_only_function.h"

namespace flox
{

class IWebSocketClient : public ISubsystem
{
 public:
  virtual ~IWebSocketClient() = default;

  virtual void onOpen(MoveOnlyFunction<void()> cb) = 0;
  virtual void onMessage(MoveOnlyFunction<void(std::string_view)> cb) = 0;
  virtual void onClose(MoveOnlyFunction<void(int, std::string_view)> cb) = 0;

  virtual void send(const std::string& data) = 0;
};

}  // namespace flox
