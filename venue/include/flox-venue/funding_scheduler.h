/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/funding_rate.h"
#include "flox-venue/messages.h"

#include <cstdint>
#include <optional>

namespace flox::venue
{

template <class Engine>
class FundingScheduler
{
 public:
  FundingScheduler(Engine& eng, int64_t fundingIntervalNs, FundingCalculator calc = FundingCalculator{})
      : eng_(eng), interval_(fundingIntervalNs), calc_(calc)
  {
  }

  // Feed the current mark/index at time nowNs. Samples the premium; when a
  // funding boundary is crossed, settles funding on the engine at the sampled
  // TWAP rate, resets the accumulator, and returns the settled rate.
  std::optional<double> onTick(int64_t nowNs, Price mark, Price index)
  {
    if (nextSettle_ < 0)
    {
      nextSettle_ = nowNs + interval_;  // first tick anchors the interval
    }
    calc_.sample(mark.toDouble(), index.toDouble());

    if (nowNs >= nextSettle_)
    {
      const double rate = calc_.intervalRate();
      eng_.applyFunding(rate, mark);
      calc_.resetSamples();
      nextSettle_ += interval_;
      ++settlements_;
      lastRate_ = rate;
      return rate;
    }
    return std::nullopt;
  }

  int64_t nextSettleNs() const noexcept { return nextSettle_; }
  uint64_t settlements() const noexcept { return settlements_; }
  double lastRate() const noexcept { return lastRate_; }

 private:
  Engine& eng_;
  int64_t interval_;
  FundingCalculator calc_;
  int64_t nextSettle_{-1};
  uint64_t settlements_{0};
  double lastRate_{0.0};
};

}  // namespace flox::venue
