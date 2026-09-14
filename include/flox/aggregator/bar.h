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

// No policy runs a time-gap detector, so a hypothetical `Gap` value would
// need one built for every bar type before anything could ever set it --
// that used to sit at 1 here and was removed rather than shipped unset
// forever. `Forced` and `Warmup` keep their original numeric values (2, 3)
// rather than sliding down to 1 and 2, because `Bar` (this `reason` byte
// included) is written to disk as a raw struct by MmapBarWriter: renumbering
// them would silently change the meaning of a byte already sitting in
// existing bar files.
enum class BarCloseReason : uint8_t
{
  Threshold = 0,  // Normal close: interval/count/volume reached
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
