/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "flox/engine/abstract_subsystem.h"

namespace flox
{

class IWebSocketClient : public ISubsystem
{
 public:
  virtual ~IWebSocketClient() = default;

  virtual void onOpen(std::move_only_function<void()> cb) = 0;
  virtual void onMessage(std::move_only_function<void(std::string_view)> cb) = 0;
  virtual void onClose(std::move_only_function<void(int, std::string_view)> cb) = 0;

  virtual void send(const std::string& data) = 0;
};

}  // namespace flox
