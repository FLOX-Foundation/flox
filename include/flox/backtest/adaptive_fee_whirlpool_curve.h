/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/orca_whirlpool_curve.h"
#include "flox/backtest/whirlpool_tick_math.h"
#include "flox/util/int/i256.h"
#include "flox/util/int/u256.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace flox
{

// Orca Whirlpool adaptive-fee state, held in the pool's Oracle account.
struct AdaptiveFeeConstants
{
  uint16_t filterPeriod{0};
  uint16_t decayPeriod{0};
  uint16_t reductionFactor{0};
  uint32_t adaptiveFeeControlFactor{0};
  uint32_t maxVolatilityAccumulator{0};
  uint16_t tickGroupSize{0};
  uint16_t majorSwapThresholdTicks{0};
};

struct AdaptiveFeeVariables
{
  uint64_t lastReferenceUpdateTimestamp{0};
  uint64_t lastMajorSwapTimestamp{0};
  uint32_t volatilityReference{0};
  int32_t tickGroupIndexReference{0};
  uint32_t volatilityAccumulator{0};
};

// The Whirlpool FeeRateManager, transcribed from the program. It evolves the
// volatility accumulator across tick groups from a time-decayed reference, and the
// fee rate is static + adaptive(volatility). The skip optimization is omitted (it
// bounds compute, not output, so the result is identical).
struct WhirlpoolFeeRateManager
{
  AdaptiveFeeConstants c;

  static constexpr uint32_t kScale = 10000;           // VOLATILITY_ACCUMULATOR_SCALE_FACTOR
  static constexpr uint32_t kReductionDen = 10000;    // REDUCTION_FACTOR_DENOMINATOR
  static constexpr uint32_t kControlDen = 100000;     // ADAPTIVE_FEE_CONTROL_FACTOR_DENOMINATOR
  static constexpr uint32_t kHardLimit = 100000;      // FEE_RATE_HARD_LIMIT (10%)
  static constexpr uint64_t kMaxReferenceAge = 3600;  // MAX_REFERENCE_AGE (1h)

  static int32_t floorDiv(int32_t a, int32_t b)
  {
    const int32_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
  }

  void updateReference(AdaptiveFeeVariables& v, int32_t tickGroupIndex, uint64_t ts) const
  {
    const uint64_t maxTs = std::max(v.lastReferenceUpdateTimestamp, v.lastMajorSwapTimestamp);
    if (ts < maxTs)
    {
      return;  // the program errors on a backwards timestamp; for replay we hold
    }
    if (ts - v.lastReferenceUpdateTimestamp > kMaxReferenceAge)
    {
      v.tickGroupIndexReference = tickGroupIndex;
      v.volatilityReference = 0;
      v.lastReferenceUpdateTimestamp = ts;
      return;
    }
    const uint64_t elapsed = ts - maxTs;
    if (elapsed < c.filterPeriod)
    {
      return;  // high-frequency trade, references unchanged
    }
    v.tickGroupIndexReference = tickGroupIndex;
    if (elapsed < c.decayPeriod)
    {
      v.volatilityReference = static_cast<uint32_t>(
          static_cast<uint64_t>(v.volatilityAccumulator) * c.reductionFactor / kReductionDen);
    }
    else
    {
      v.volatilityReference = 0;
    }
    v.lastReferenceUpdateTimestamp = ts;
  }

  void updateVolatilityAccumulator(AdaptiveFeeVariables& v, int32_t tickGroupIndex) const
  {
    const uint64_t delta = static_cast<uint64_t>(
        std::abs(static_cast<int64_t>(v.tickGroupIndexReference) - tickGroupIndex));
    const uint64_t va = static_cast<uint64_t>(v.volatilityReference) + delta * kScale;
    v.volatilityAccumulator = static_cast<uint32_t>(std::min<uint64_t>(va, c.maxVolatilityAccumulator));
  }

  uint32_t computeAdaptiveFeeRate(uint32_t volatilityAccumulator) const
  {
    const uint64_t crossed = static_cast<uint64_t>(volatilityAccumulator) * c.tickGroupSize;
    const u256 num = u256(c.adaptiveFeeControlFactor) * (u256(crossed) * u256(crossed));
    const u256 den = u256(kControlDen) * u256(kScale) * u256(kScale);
    const u256 fee = num.isZero() ? u256(0) : (num - u256(1)) / den + u256(1);  // ceil
    return fee > u256(kHardLimit) ? kHardLimit : static_cast<uint32_t>(fee.w[0]);
  }

  uint32_t totalFeeRate(uint32_t staticFee, const AdaptiveFeeVariables& v) const
  {
    const uint32_t total = staticFee + computeAdaptiveFeeRate(v.volatilityAccumulator);
    return total > kHardLimit ? kHardLimit : total;
  }
};

// An adaptive-fee Whirlpool: OrcaWhirlpoolCurve's concentrated-liquidity swap with
// the volatility-driven fee. Each step is bounded to the next tick-group boundary
// so the fee is constant within it; the accumulator updates as groups are crossed.
// The current tick and the swap timestamp come from the pool, the fee state from
// the Oracle account.
class AdaptiveFeeWhirlpoolCurve : public OrcaWhirlpoolCurve
{
 public:
  AdaptiveFeeWhirlpoolCurve(u256 sqrtPriceX64, u256 liquidity, uint32_t staticFeeRate,
                            std::vector<ClTick> ticks, int32_t currentTickIndex, uint64_t timestamp,
                            AdaptiveFeeConstants constants, AdaptiveFeeVariables variables)
      : OrcaWhirlpoolCurve(sqrtPriceX64, liquidity, staticFeeRate, std::move(ticks)),
        _currentTick(currentTickIndex),
        _timestamp(timestamp),
        _mgr{constants},
        _v(variables)
  {
  }

  const WhirlpoolFeeRateManager& feeManager() const { return _mgr; }

  u256 amountOut(std::size_t i, std::size_t j, const u256& amountIn) const override
  {
    (void)j;
    u256 sqrt = _sqrtP;
    u256 L = _L;
    AdaptiveFeeVariables v = _v;
    return runAdaptive(i == 0, amountIn, sqrt, L, v);
  }

  u256 applySwap(std::size_t i, std::size_t j, const u256& amountIn) override
  {
    (void)j;
    u256 sqrt = _sqrtP;
    u256 L = _L;
    AdaptiveFeeVariables v = _v;
    const u256 out = runAdaptive(i == 0, amountIn, sqrt, L, v);
    _sqrtP = sqrt;
    _L = L;
    _v = v;
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<AdaptiveFeeWhirlpoolCurve>(*this);
  }

 private:
  static constexpr int32_t kMinTick = -443636;
  static constexpr int32_t kMaxTick = 443636;

  u256 boundaryAt(int32_t tickGroupIndex, bool aToB) const
  {
    int32_t t = aToB ? tickGroupIndex * _mgr.c.tickGroupSize
                     : tickGroupIndex * _mgr.c.tickGroupSize + _mgr.c.tickGroupSize;
    t = std::clamp(t, kMinTick, kMaxTick);
    return whirlpool_tick::sqrtPriceFromTick(t);
  }

  u256 runAdaptive(bool aToB, const u256& amountIn, u256& sqrt, u256& L,
                   AdaptiveFeeVariables& v) const
  {
    if (amountIn.isZero())
    {
      return u256(0);
    }
    int32_t tgi = WhirlpoolFeeRateManager::floorDiv(_currentTick, _mgr.c.tickGroupSize);
    _mgr.updateReference(v, tgi, _timestamp);

    u256 rem = amountIn;
    u256 out(0);
    const u256 limit = aToB ? _minSqrt + u256(1) : _maxSqrt - u256(1);
    for (int guard = 0; guard < 8192; ++guard)
    {
      if (rem.isZero() || sqrt == limit || L.isZero())
      {
        break;
      }
      bool hasTick = false;
      u256 tickSqrt(0);
      i256 net(0);
      findNextTick(sqrt, aToB, hasTick, tickSqrt, net);
      u256 tickTarget = limit;
      if (hasTick)
      {
        tickTarget = aToB ? (tickSqrt > limit ? tickSqrt : limit)
                          : (tickSqrt < limit ? tickSqrt : limit);
      }
      // Bound the step to the next tick-group boundary so the fee is constant
      // within it. When there is no adaptive fee (control factor 0) the program
      // skips the bounding, so the swap is exactly the static OrcaWhirlpoolCurve.
      u256 target = tickTarget;
      if (_mgr.c.adaptiveFeeControlFactor != 0)
      {
        const u256 boundary = boundaryAt(tgi, aToB);
        target = aToB ? (tickTarget > boundary ? tickTarget : boundary)
                      : (tickTarget < boundary ? tickTarget : boundary);
      }

      _mgr.updateVolatilityAccumulator(v, tgi);
      const uint32_t fee = _mgr.totalFeeRate(_fee, v);

      u256 amountInStep(0), amountOutStep(0), feeAmount(0);
      const u256 sqrtNext =
          computeStep(sqrt, target, L, rem, aToB, fee, amountInStep, amountOutStep, feeAmount);
      rem = rem - (amountInStep + feeAmount);
      out = out + amountOutStep;

      if (hasTick && sqrtNext == tickSqrt)
      {
        const i256 dL = aToB ? -net : net;
        L = dL.neg ? (L - dL.mag) : (L + dL.mag);
      }
      sqrt = sqrtNext;
      tgi += aToB ? -1 : 1;  // advance_tick_group, every step
    }
    return out;
  }

  int32_t _currentTick;
  uint64_t _timestamp;
  WhirlpoolFeeRateManager _mgr;
  AdaptiveFeeVariables _v;
};

}  // namespace flox
