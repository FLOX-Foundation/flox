/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <chrono>
#include "flox/common.h"

namespace flox
{

struct Trade
{
  SymbolId symbol{};
  Price price{};
  Quantity quantity{};
  bool isBuy{false};
  std::chrono::steady_clock::time_point timestamp{};
};

}  // namespace flox
