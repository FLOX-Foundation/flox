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

// Outage pathology. The default `Total` matches the T023 behaviour
// (everything is dropped/buffered). Real venues exhibit finer modes:
//
//   SubmitOnlyDown    — cancels still work, submits buffered. Common
//                       during venue-side rolling restarts.
//   CancelOnlyDown    — submits still work, cancels buffered. Same
//                       failure mode in the opposite direction.
//   SlowDegradation   — every submit/cancel/replace ack latency is
//                       multiplied by `degradationLatencyMultiplier`.
//                       Market data still flows.
//   StaleBook         — onBookUpdate is dropped during the window;
//                       trades continue to fire. Orders match against
//                       the last book snapshot, modelling the
//                       "frozen book" Binance / Bybit pathology.
//   WrongSideRecovery — on recovery, the first mark feed-through is
//                       offset by `wrongSideRecoveryBps` (signed)
//                       basis points to model the venue catching up
//                       internally while it was down.
enum class OutageType : uint8_t
{
  Total = 0,
  SubmitOnlyDown = 1,
  CancelOnlyDown = 2,
  SlowDegradation = 3,
  StaleBook = 4,
  WrongSideRecovery = 5,
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
    OutageType outageType{OutageType::Total};
    // SlowDegradation: multiply submit/cancel/replace ack latency by
    // this factor during the window. Other outage types ignore it.
    double degradationLatencyMultiplier{1.0};
    // WrongSideRecovery: signed bps offset to apply to the first mark
    // feed-through after the outage ends. Positive == mark drifts up;
    // negative == mark drifts down.
    double wrongSideRecoveryBps{0.0};
  };

  VenueAvailability() = default;

  VenueAvailability& scheduleOutage(int64_t startNs, int64_t durationNs,
                                    OnOutage policy = OnOutage::CANCEL_ALL,
                                    int64_t gtcTtlNs = 0);

  // Extended schedule with pathology selection. The base policy still
  // governs what happens to open orders at outage start; the
  // `outageType` controls which actions and feeds are degraded.
  VenueAvailability& scheduleOutageEx(int64_t startNs, int64_t durationNs,
                                      OutageType outageType,
                                      OnOutage policy = OnOutage::CANCEL_ALL,
                                      int64_t gtcTtlNs = 0,
                                      double degradationLatencyMultiplier = 1.0,
                                      double wrongSideRecoveryBps = 0.0);

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

  // Per-action / per-feed gates. For Total-mode outages they all
  // return false; partial outages return true for the feeds they
  // don't degrade. SlowDegradation lets every action through; the
  // caller multiplies latency by `latencyMultiplier(now)`.
  bool submitsAllowed(int64_t nowNs);
  bool cancelsAllowed(int64_t nowNs);
  bool bookUpdatesAllowed(int64_t nowNs);
  bool tradesAllowed(int64_t nowNs);
  // Effective latency multiplier at nowNs (1.0 when no degradation).
  double latencyMultiplier(int64_t nowNs);
  // Pop the wrong-side-recovery offset accumulated during outages
  // that just ended. Returns 0.0 if no WrongSideRecovery outage
  // recently transitioned to up. Idempotent: subsequent calls return
  // 0 until the next WrongSideRecovery outage ends.
  double consumeWrongSideRecoveryBps();

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
  // Accumulator for WrongSideRecovery: when a WrongSideRecovery
  // outage transitions from active to ended, its bps is added here.
  // Cleared by consumeWrongSideRecoveryBps().
  double _pendingWrongSideRecoveryBps{0.0};
  // Track outages we've already accounted for at recovery so the
  // wrong-side bps is not double-charged across multiple polls.
  int64_t _lastConsumedRecoveryEndNs{INT64_MIN};
};

}  // namespace flox
