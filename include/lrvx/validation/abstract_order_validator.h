/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/engine/abstract_subsystem.h"
#include "lrvx/execution/order.h"

#include <string>

namespace lrvx
{

class IOrderValidator : public ISubsystem
{
 public:
  virtual ~IOrderValidator() = default;

  virtual bool validate(const Order& order, std::string& reason) const = 0;
};

}  // namespace lrvx