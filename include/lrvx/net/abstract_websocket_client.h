/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <string>
#include <string_view>

#include "lrvx/engine/abstract_subsystem.h"
#include "lrvx/util/base/move_only_function.h"

namespace lrvx
{

class IWebSocketClient : public ISubsystem
{
 public:
  virtual ~IWebSocketClient() = default;

  virtual void onOpen(MoveOnlyFunction<void()> cb) = 0;
  virtual void onMessage(MoveOnlyFunction<void(std::string_view)> cb) = 0;
  virtual void onClose(MoveOnlyFunction<void(int, std::string_view)> cb) = 0;

  // Returns whether the frame was handed to the transport successfully.
  // Implementations must not discard this: a caller that needs the frame
  // delivered (subscription/resubscription control frames in particular)
  // has no other way to learn that a send silently failed.
  virtual bool send(const std::string& data) = 0;
};

}  // namespace lrvx
