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

#include <chrono>

namespace flox
{

struct Candle
{
  Price open;
  Price high;
  Price low;
  Price close;
  Volume volume;
  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point endTime;

  Candle() = default;

  Candle(std::chrono::steady_clock::time_point ts, Price price, Volume qty)
      : open(price),
        high(price),
        low(price),
        close(price),
        volume(qty),
        startTime(ts),
        endTime(ts)
  {
  }
};

}  // namespace flox