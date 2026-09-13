/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/aggregation_policy.h"

namespace flox
{

class VolumeBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::Volume;

  // threshold is in raw volume units (scaled by Volume::Scale)
  explicit constexpr VolumeBarPolicy(int64_t thresholdRaw) noexcept
      : _thresholdRaw(thresholdRaw)
  {
  }

  // Convenience: threshold from double
  static VolumeBarPolicy fromDouble(double threshold) noexcept
  {
    return VolumeBarPolicy(Volume::fromDouble(threshold).raw());
  }

  constexpr uint64_t param() const noexcept
  {
    // Same units as TimeframeId::volume(threshold): the notional threshold
    // as passed to fromDouble()/the constructor (e.g. 1'000'000 for "$1M
    // volume"), not the internal Volume::Scale-scaled fixed-point _raw
    // representation. BarAggregator<VolumeBarPolicy> and BarMatrix must
    // agree on this value or a matrix configured with
    // TimeframeId::volume(threshold) never finds the bars this policy
    // emits (they used to disagree: this used to divide by 1000 instead
    // of Volume::Scale, and also silently truncated to 32 bits).
    return static_cast<uint64_t>(_thresholdRaw / Volume::Scale);
  }

  [[nodiscard]] bool shouldClose(const TradeEvent& /*trade*/, const Bar& bar) const noexcept
  {
    return bar.volume.raw() >= _thresholdRaw;
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept
  {
    updateOHLCV(trade, bar);
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept
  {
    initBarFromTrade(trade, bar);
  }

 private:
  int64_t _thresholdRaw;
};

static_assert(BarPolicy<VolumeBarPolicy>, "VolumeBarPolicy must satisfy BarPolicy concept");

}  // namespace flox
