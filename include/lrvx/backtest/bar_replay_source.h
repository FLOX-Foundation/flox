/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/aggregator/bar.h"
#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/aggregator/timeframe.h"
#include "lrvx/common.h"

#include <functional>
#include <vector>

namespace lrvx
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

}  // namespace lrvx
