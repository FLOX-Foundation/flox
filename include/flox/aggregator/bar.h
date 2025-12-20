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

#include <cstdint>

namespace flox
{

enum class BarType : uint8_t
{
  Time,
  Tick,
  Volume,
  Renko,
  Range
};

enum class BarCloseReason : uint8_t
{
  Threshold,  // Normal close: interval/count/volume reached
  Gap,        // Gap in data: new bar started due to time gap
  Forced,     // Forced close: stop() called or manual flush
  Warmup      // Historical warmup bar
};

struct Bar
{
  Price open{};
  Price high{};
  Price low{};
  Price close{};
  Volume volume{};
  Volume buyVolume{};     // For delta calculation (buyVolume - sellVolume)
  Quantity tradeCount{};  // Number of trades in this bar
  TimePoint startTime{};
  TimePoint endTime{};
  BarCloseReason reason{BarCloseReason::Threshold};

  Bar() = default;

  Bar(TimePoint ts, Price price, Volume vol)
      : open(price),
        high(price),
        low(price),
        close(price),
        volume(vol),
        buyVolume{},
        tradeCount(Quantity::fromRaw(1)),
        startTime(ts),
        endTime(ts)
  {
  }
};

}  // namespace flox
