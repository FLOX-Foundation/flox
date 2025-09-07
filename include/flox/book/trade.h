/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"
#include "flox/util/base/time.h"

namespace flox
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

}  // namespace flox
