/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/connector/abstract_exchange_connector.h"

#include <optional>

namespace flox
{

struct TimeRange
{
  int64_t start_ns{0};
  int64_t end_ns{0};
};

struct ReplaySpeed
{
  double multiplier{0.0};

  static ReplaySpeed realtime() { return {1.0}; }
  static ReplaySpeed fast(double x) { return {x}; }
  static ReplaySpeed max() { return {0.0}; }

  bool isMax() const { return multiplier <= 0.0; }
  bool isRealtime() const { return multiplier == 1.0; }
};

class IReplaySource : public IExchangeConnector
{
 public:
  virtual ~IReplaySource() = default;

  bool isLive() const { return false; }

  virtual std::optional<TimeRange> dataRange() const = 0;
  virtual void setSpeed(ReplaySpeed speed) = 0;
  virtual bool seekTo(int64_t timestamp_ns) = 0;
  virtual bool isFinished() const = 0;
  virtual int64_t currentPosition() const = 0;
};

}  // namespace flox
