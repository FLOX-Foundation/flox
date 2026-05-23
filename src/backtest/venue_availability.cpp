/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/venue_availability.h"

#include <algorithm>
#include <cmath>

namespace flox
{

namespace
{
constexpr int64_t kNsPerDay = static_cast<int64_t>(86400) * 1'000'000'000LL;
}

VenueAvailability& VenueAvailability::scheduleOutage(int64_t startNs, int64_t durationNs,
                                                     OnOutage policy, int64_t gtcTtlNs)
{
  if (durationNs <= 0)
  {
    return *this;
  }
  _outages.push_back(Outage{.startNs = startNs,
                            .endNs = startNs + durationNs,
                            .policy = policy,
                            .gtcTtlNs = gtcTtlNs});
  std::sort(_outages.begin(), _outages.end(),
            [](const Outage& a, const Outage& b)
            { return a.startNs < b.startNs; });
  return *this;
}

VenueAvailability& VenueAvailability::scheduleOutageEx(int64_t startNs, int64_t durationNs,
                                                       OutageType outageType,
                                                       OnOutage policy, int64_t gtcTtlNs,
                                                       double degradationLatencyMultiplier,
                                                       double wrongSideRecoveryBps)
{
  if (durationNs <= 0)
  {
    return *this;
  }
  Outage o;
  o.startNs = startNs;
  o.endNs = startNs + durationNs;
  o.policy = policy;
  o.gtcTtlNs = gtcTtlNs;
  o.outageType = outageType;
  o.degradationLatencyMultiplier = degradationLatencyMultiplier;
  o.wrongSideRecoveryBps = wrongSideRecoveryBps;
  _outages.push_back(o);
  std::sort(_outages.begin(), _outages.end(),
            [](const Outage& a, const Outage& b)
            { return a.startNs < b.startNs; });
  return *this;
}

VenueAvailability& VenueAvailability::autoRandomOutages(double perDay,
                                                        int64_t meanDurationNs,
                                                        OnOutage policy, uint64_t seed)
{
  _random = perDay > 0.0 && meanDurationNs > 0;
  _perDay = perDay;
  _meanDurationNs = meanDurationNs;
  _randomPolicy = policy;
  _rng.seed(seed);
  return *this;
}

void VenueAvailability::realiseUpTo(int64_t nowNs)
{
  if (!_random)
  {
    return;
  }
  if (_randomRealisedNs == INT64_MIN)
  {
    _randomRealisedNs = 0;
  }
  // Poisson process: inter-arrival is Exponential(lambda) where
  // lambda = perDay / kNsPerDay events per nanosecond. Sample
  // arrivals until we pass nowNs.
  const double lambdaPerNs = _perDay / static_cast<double>(kNsPerDay);
  if (lambdaPerNs <= 0.0)
  {
    _randomRealisedNs = nowNs;
    return;
  }
  std::uniform_real_distribution<double> u(0.0, 1.0);
  int64_t t = _randomRealisedNs;
  while (t < nowNs)
  {
    const double dt = -std::log(std::max(u(_rng), 1e-12)) / lambdaPerNs;
    if (!std::isfinite(dt))
    {
      break;
    }
    t += static_cast<int64_t>(dt);
    if (t >= nowNs)
    {
      break;
    }
    const double dur = -std::log(std::max(u(_rng), 1e-12)) *
                       static_cast<double>(_meanDurationNs);
    const int64_t durNs = static_cast<int64_t>(dur);
    if (durNs > 0)
    {
      _outages.push_back(Outage{.startNs = t,
                                .endNs = t + durNs,
                                .policy = _randomPolicy,
                                .gtcTtlNs = 0});
    }
  }
  std::sort(_outages.begin(), _outages.end(),
            [](const Outage& a, const Outage& b)
            { return a.startNs < b.startNs; });
  _randomRealisedNs = nowNs;
}

const VenueAvailability::Outage* VenueAvailability::activeOutage(int64_t nowNs)
{
  realiseUpTo(nowNs);
  for (const auto& o : _outages)
  {
    if (o.startNs <= nowNs && nowNs < o.endNs)
    {
      return &o;
    }
    if (o.startNs > nowNs)
    {
      break;  // sorted
    }
  }
  return nullptr;
}

bool VenueAvailability::isUp(int64_t nowNs)
{
  const Outage* o = activeOutage(nowNs);
  // Partial-outage variants are still "up" at the binary level —
  // their finer behaviour is exposed through the per-action / per-
  // feed gates below. Total outages remain hard-down.
  if (o == nullptr)
  {
    return true;
  }
  switch (o->outageType)
  {
    case OutageType::Total:
    case OutageType::WrongSideRecovery:
      return false;
    default:
      return true;
  }
}

bool VenueAvailability::submitsAllowed(int64_t nowNs)
{
  const Outage* o = activeOutage(nowNs);
  if (o == nullptr)
  {
    return true;
  }
  switch (o->outageType)
  {
    case OutageType::Total:
    case OutageType::WrongSideRecovery:
    case OutageType::SubmitOnlyDown:
      return false;
    default:
      return true;
  }
}

bool VenueAvailability::cancelsAllowed(int64_t nowNs)
{
  const Outage* o = activeOutage(nowNs);
  if (o == nullptr)
  {
    return true;
  }
  switch (o->outageType)
  {
    case OutageType::Total:
    case OutageType::WrongSideRecovery:
    case OutageType::CancelOnlyDown:
      return false;
    default:
      return true;
  }
}

bool VenueAvailability::bookUpdatesAllowed(int64_t nowNs)
{
  const Outage* o = activeOutage(nowNs);
  if (o == nullptr)
  {
    return true;
  }
  switch (o->outageType)
  {
    case OutageType::Total:
    case OutageType::WrongSideRecovery:
    case OutageType::StaleBook:
      return false;
    default:
      return true;
  }
}

bool VenueAvailability::tradesAllowed(int64_t nowNs)
{
  const Outage* o = activeOutage(nowNs);
  if (o == nullptr)
  {
    return true;
  }
  switch (o->outageType)
  {
    case OutageType::Total:
    case OutageType::WrongSideRecovery:
      return false;
    default:
      return true;
  }
}

double VenueAvailability::latencyMultiplier(int64_t nowNs)
{
  const Outage* o = activeOutage(nowNs);
  if (o == nullptr || o->outageType != OutageType::SlowDegradation)
  {
    return 1.0;
  }
  return o->degradationLatencyMultiplier;
}

double VenueAvailability::consumeWrongSideRecoveryBps()
{
  const double v = _pendingWrongSideRecoveryBps;
  _pendingWrongSideRecoveryBps = 0.0;
  return v;
}

bool VenueAvailability::consumeRecoveryEdge(int64_t nowNs)
{
  const bool up = isUp(nowNs);
  const bool edge = up && !_wasUpLast;
  _wasUpLast = up;
  if (edge)
  {
    // Walk completed outages whose endNs is just before nowNs; for
    // each WrongSideRecovery outage we haven't accounted for yet,
    // accumulate its bps into the pending offset.
    for (const auto& o : _outages)
    {
      if (o.endNs > nowNs)
      {
        break;  // sorted by start; once we pass nowNs we're done
      }
      if (o.outageType == OutageType::WrongSideRecovery &&
          o.endNs > _lastConsumedRecoveryEndNs)
      {
        _pendingWrongSideRecoveryBps += o.wrongSideRecoveryBps;
        _lastConsumedRecoveryEndNs = o.endNs;
      }
    }
  }
  return edge;
}

}  // namespace flox
