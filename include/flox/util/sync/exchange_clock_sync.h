/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/common.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace flox
{

template <size_t MaxExchanges = 8>
class ExchangeClockSync
{
 public:
  struct ClockEstimate
  {
    int64_t offsetNs{0};      // exchange - local (nanoseconds)
    int64_t confidenceNs{0};  // ± uncertainty (2 sigma)
    int64_t latencyNs{0};     // one-way estimate
    uint32_t sampleCount{0};
  };

  bool recordSample(ExchangeId exchange,
                    int64_t localSendNs,
                    int64_t exchangeNs,
                    int64_t localRecvNs)
  {
    if (exchange >= MaxExchanges) [[unlikely]]
    {
      return false;
    }

    int64_t rtt = localRecvNs - localSendNs;

    // Reject impossible values
    if (rtt <= 0 || rtt > 10'000'000'000LL) [[unlikely]]
    {
      return false;  // RTT > 10s or negative
    }
    if (exchangeNs < localSendNs - 3600'000'000'000LL) [[unlikely]]
    {
      return false;  // Clock drift > 1 hour
    }

    auto& s = _states[exchange];
    int64_t oneWay = rtt / 2;
    int64_t rawOffset = exchangeNs - (localSendNs + oneWay);

    // EMA update (alpha = 0.1)
    if (s.sampleCount == 0)
    {
      s.offsetNs = rawOffset;
      s.latencyNs = oneWay;
      s.varianceAcc = 0;
    }
    else
    {
      s.offsetNs = (s.offsetNs * 9 + rawOffset) / 10;
      s.latencyNs = (s.latencyNs * 9 + oneWay) / 10;

      // Variance accumulator for confidence interval
      int64_t diff = rawOffset - s.offsetNs;
      s.varianceAcc = (s.varianceAcc * 9 + diff * diff) / 10;
    }

    ++s.sampleCount;
    return true;
  }

  ClockEstimate estimate(ExchangeId exchange) const
  {
    if (exchange >= MaxExchanges)
    {
      return {};
    }

    const auto& s = _states[exchange];
    return {
        .offsetNs = s.offsetNs,
        .confidenceNs =
            static_cast<int64_t>(2 * std::sqrt(static_cast<double>(s.varianceAcc))),
        .latencyNs = s.latencyNs,
        .sampleCount = s.sampleCount};
  }

  int64_t toLocalTimeNs(ExchangeId exchange, int64_t exchangeNs) const
  {
    if (exchange >= MaxExchanges)
    {
      return exchangeNs;
    }
    return exchangeNs - _states[exchange].offsetNs;
  }

  int64_t toExchangeTimeNs(ExchangeId exchange, int64_t localNs) const
  {
    if (exchange >= MaxExchanges)
    {
      return localNs;
    }
    return localNs + _states[exchange].offsetNs;
  }

  bool hasSync(ExchangeId exchange) const
  {
    if (exchange >= MaxExchanges)
    {
      return false;
    }
    return _states[exchange].sampleCount > 0;
  }

  static constexpr uint32_t kMinSamplesForReliable = 10;

  bool hasReliableSync(ExchangeId exchange) const
  {
    if (exchange >= MaxExchanges)
    {
      return false;
    }
    return _states[exchange].sampleCount >= kMinSamplesForReliable;
  }

  void reset(ExchangeId exchange)
  {
    if (exchange < MaxExchanges)
    {
      _states[exchange] = ClockState{};
    }
  }

  void resetAll() { _states = {}; }

 private:
  struct ClockState
  {
    int64_t offsetNs{0};
    int64_t latencyNs{0};
    int64_t varianceAcc{0};
    uint32_t sampleCount{0};
  };

  std::array<ClockState, MaxExchanges> _states{};
};

}  // namespace flox
