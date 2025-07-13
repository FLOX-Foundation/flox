/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/book/candle.h"
#include "flox/common.h"
#include "flox/engine/abstract_market_data_subscriber.h"

namespace flox
{

struct CandleEvent
{
  using Listener = IMarketDataSubscriber;

  SymbolId symbol{};
  Candle candle{};

  uint64_t tickSequence = 0;
};

}  // namespace flox
