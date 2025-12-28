/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bar.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/aggregator/timeframe.h"
#include "flox/common.h"

#include <functional>
#include <vector>

namespace flox
{

class IBarReplaySource
{
 public:
  virtual ~IBarReplaySource() = default;

  virtual size_t replay(std::function<void(const BarEvent&)> callback) = 0;
  virtual std::vector<TimeframeId> timeframes() const = 0;
  virtual size_t totalBars() const = 0;
  virtual std::pair<TimePoint, TimePoint> timeRange() const = 0;
};

}  // namespace flox
