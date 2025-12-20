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

  [[nodiscard]] constexpr uint32_t param() const noexcept
  {
    // Return threshold in scaled units (fits in 28 bits for reasonable values)
    return static_cast<uint32_t>(_thresholdRaw / 1000);  // Compress for TimeframeId
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
