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

#include <cstdint>
#include <random>
#include <vector>

namespace flox
{

// Venue behaviour for orders that are open when an outage begins.
enum class OnOutage : uint8_t
{
  CANCEL_ALL = 0,        // all open orders cancel at outage start
  HOLD = 1,              // orders stay; no events during outage
  EXPIRE_GTC_AFTER = 2,  // drop orders past their venue TTL during outage
};

// Models venue downtime: scheduled maintenance windows and random
// disconnects. SimulatedExecutor consults `isUp(nowNs)` before
// processing submit/cancel/replace; requests issued during an outage
// are buffered and flushed at the recovery edge in arrival order.
//
// Market data callbacks fed into the simulator are silently dropped
// during an outage so that the strategy sees a feed gap. Recovery
// emits queued requests in FIFO order, subject to any attached rate
// limiter.
class VenueAvailability
{
 public:
  struct Outage
  {
    int64_t startNs{0};
    int64_t endNs{0};
    OnOutage policy{OnOutage::CANCEL_ALL};
    int64_t gtcTtlNs{0};  // EXPIRE_GTC_AFTER only; 0 means no expiry
  };

  VenueAvailability() = default;

  VenueAvailability& scheduleOutage(int64_t startNs, int64_t durationNs,
                                    OnOutage policy = OnOutage::CANCEL_ALL,
                                    int64_t gtcTtlNs = 0);

  // Random outages drawn from a Poisson process with mean rate
  // `perDay` outages per UTC day; durations exponentially distributed
  // with mean `meanDurationNs`. Sampling is deterministic given
  // `seed`. Random outages are realised lazily as the simulator
  // advances time via `isUp(nowNs)`.
  VenueAvailability& autoRandomOutages(double perDay, int64_t meanDurationNs,
                                       OnOutage policy = OnOutage::CANCEL_ALL,
                                       uint64_t seed = 0xC0FFEEULL);

  // True when there is no scheduled or sampled outage active at
  // nowNs. Realises random outages up to `nowNs` on first call.
  bool isUp(int64_t nowNs);

  // Active outage at `nowNs`, or nullptr if up. Realises random
  // outages up to `nowNs`.
  const Outage* activeOutage(int64_t nowNs);

  // Edge detection helper: returns true exactly once after the
  // venue transitions from down to up, then resets. Strategies and
  // the simulator use this to flush buffered requests at recovery.
  bool consumeRecoveryEdge(int64_t nowNs);

  const std::vector<Outage>& outages() const { return _outages; }

 private:
  void realiseUpTo(int64_t nowNs);

  std::vector<Outage> _outages;  // sorted by startNs after each realise call
  bool _random{false};
  double _perDay{0.0};
  int64_t _meanDurationNs{0};
  OnOutage _randomPolicy{OnOutage::CANCEL_ALL};
  std::mt19937_64 _rng{0xC0FFEEULL};
  int64_t _randomRealisedNs{INT64_MIN};
  bool _wasUpLast{true};
};

}  // namespace flox
