/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/common.h"
#include "lrvx/util/base/time.h"

namespace lrvx
{

struct Trade
{
  SymbolId symbol{};
  InstrumentType instrument = InstrumentType::Spot;
  Price price{};
  Quantity quantity{};
  bool isBuy{false};
  UnixNanos exchangeTsNs{0};
};

}  // namespace lrvx
