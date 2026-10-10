/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include "lrvx/engine/abstract_subsystem.h"
#include "lrvx/util/base/move_only_function.h"

namespace lrvx
{

class ITransport : public ISubsystem
{
 public:
  virtual ~ITransport() = default;

  virtual void post(std::string_view url,
                    std::string_view body,
                    const std::vector<std::pair<std::string_view, std::string_view>>& headers,
                    MoveOnlyFunction<void(std::string_view)> onSuccess,
                    MoveOnlyFunction<void(std::string_view)> onError) = 0;
};

}  // namespace lrvx
