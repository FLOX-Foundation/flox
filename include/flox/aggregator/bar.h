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
  Range,
  HeikinAshi,
  BpsRange
};

// The numeric values are fixed: `Bar` (this `reason` byte included) is
// written to disk as a raw struct by MmapBarWriter, so renumbering an
// enumerator would silently change the meaning of a byte already sitting in
// existing bar files. 1 was left free for `Gap` when no policy could set it;
// RenkoBarPolicy::closeAndReopen now does, and the C ABI has documented the
// value as Gap all along (FloxBarData::close_reason in capi/flox_capi.h).
enum class BarCloseReason : uint8_t
{
  Threshold = 0,  // Normal close: interval/count/volume reached
  Gap = 1,        // A price jump too wide to walk brick by brick -- see RenkoBarPolicy::kMaxGapBricks
  Forced = 2,     // Forced close: stop() called or manual flush
  Warmup = 3      // Set by the caller, not the engine -- see BarMatrix::warmup()
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
