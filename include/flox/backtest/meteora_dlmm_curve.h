/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/ntoken_curve.h"
#include "flox/util/int/u256.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace flox
{

// A populated DLMM bin: its id and the token reserves it holds. Liquidity sits in
// discrete price bins, each a constant-sum segment at the bin's fixed price.
struct DlmmBin
{
  int32_t id{0};
  u256 amountX{0};
  u256 amountY{0};
};

// The pool's adaptive-fee parameters (static) and volatility state (variable),
// from the LbPair account.
struct DlmmFeeParams
{
  uint32_t baseFactor{0};
  uint8_t baseFeePowerFactor{0};
  uint32_t variableFeeControl{0};
  uint16_t filterPeriod{0};
  uint16_t decayPeriod{0};
  uint16_t reductionFactor{0};
  uint32_t maxVolatilityAccumulator{0};
};

struct DlmmVolatility
{
  uint32_t volatilityAccumulator{0};
  uint32_t volatilityReference{0};
  int32_t indexReference{0};
  uint64_t lastUpdateTimestamp{0};
};

// Meteora DLMM (Liquidity Book) pool on Solana, exact in integer math. Not a curve
// in the AMM sense: liquidity is in discrete price bins, each filled constant-sum
// at its own price (1 + bin_step/1e4)^id in Q64.64, and a swap walks bins from the
// active one outward. The fee is dynamic -- a base fee plus a variable fee that
// grows with a volatility accumulator as the swap crosses bins from a time-decayed
// reference -- so it is priced per bin. Transcribed from the program's quote
// (MM-liquidity exact-in, fee-on-input; limit orders and the fee-on-output mode are
// separate features, not modelled here). token X is index 0, token Y is index 1;
// an X-in swap (swap_for_y) moves the active bin down.
class MeteoraDlmmCurve : public INTokenCurve
{
 public:
  MeteoraDlmmCurve(int32_t activeId, uint16_t binStep, std::vector<DlmmBin> bins,
                   DlmmFeeParams params, DlmmVolatility volatility, uint64_t timestamp)
      : _activeId(activeId), _binStep(binStep), _params(params), _vol(volatility), _ts(timestamp)
  {
    for (const DlmmBin& b : bins)
    {
      _bins[b.id] = {b.amountX, b.amountY};
      _minId = _bins.size() == 1 ? b.id : std::min(_minId, b.id);
      _maxId = _bins.size() == 1 ? b.id : std::max(_maxId, b.id);
    }
  }

  // (1 + bin_step/1e4)^id in Q64.64, the program's bin price.
  u256 binPriceAt(int32_t id) const { return binPrice(id); }

  std::size_t tokenCount() const override { return 2; }

  // The total reserves across all bins, so the interface has a composition.
  const std::vector<u256>& balances() const override
  {
    _bal.assign(2, u256(0));
    for (const auto& [id, r] : _bins)
    {
      _bal[0] = _bal[0] + r.first;
      _bal[1] = _bal[1] + r.second;
    }
    return _bal;
  }

  u256 amountOut(std::size_t i, std::size_t j, const u256& amountIn) const override
  {
    (void)j;
    int32_t active = _activeId;
    DlmmVolatility v = _vol;
    return runSwap(i == 0, amountIn, active, v);
  }

  u256 applySwap(std::size_t i, std::size_t j, const u256& amountIn) override
  {
    (void)j;
    int32_t active = _activeId;
    DlmmVolatility v = _vol;
    const u256 out = runSwap(i == 0, amountIn, active, v, /*mutate*/ true);
    _activeId = active;
    _vol = v;
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<MeteoraDlmmCurve>(*this);
  }

 private:
  static constexpr uint64_t kBasisPointMax = 10000;
  static constexpr uint64_t kFeePrecision = 1000000000;  // 1e9
  static constexpr uint64_t kMaxFeeRate = 100000000;     // 1e8 (10%)

  static u256 q64() { return u256::fromDec("18446744073709551616"); }  // 2^64
  static u256 u128max() { return u256::fromDec("340282366920938463463374607431768211455"); }

  // get_price_from_id: (1 + bin_step/1e4)^id, Q64.64.
  u256 binPrice(int32_t id) const
  {
    const u256 bps = u256(_binStep) * q64() / u256(kBasisPointMax);
    return pow(q64() + bps, id);
  }

  // The Liquidity Book Q64.64 power, with the program's invert trick.
  static u256 pow(u256 base, int32_t exp)
  {
    if (exp == 0)
    {
      return q64();
    }
    bool invert = exp < 0;
    uint32_t e = invert ? static_cast<uint32_t>(-static_cast<int64_t>(exp))
                        : static_cast<uint32_t>(exp);
    u256 squared = base;
    u256 result = q64();
    if (squared >= result)
    {
      squared = u128max() / squared;
      invert = !invert;
    }
    for (int b = 0; b < 19; ++b)
    {
      if (b > 0)
      {
        squared = squared * squared / q64();
      }
      if (e & (1u << b))
      {
        result = result * squared / q64();
      }
    }
    if (result.isZero())
    {
      return u256(0);
    }
    return invert ? u128max() / result : result;
  }

  static u256 ceilDiv(const u256& a, const u256& b)
  {
    return a.isZero() ? u256(0) : (a - u256(1)) / b + u256(1);
  }

  // Bin::get_amount_out / get_amount_in, constant-sum at the bin price.
  static u256 amountOutAtBin(const u256& amtIn, const u256& price, bool swapForY)
  {
    return swapForY ? mulDiv(price, amtIn, q64()) : mulDiv(amtIn, q64(), price);  // floor
  }
  static u256 amountInAtBin(const u256& amtOut, const u256& price, bool swapForY)
  {
    return swapForY ? ceilDiv(amtOut * q64(), price) : mulDivUp(amtOut, price, q64());  // up
  }

  uint64_t totalFeeRate(const DlmmVolatility& v) const
  {
    const uint64_t baseFee = static_cast<uint64_t>(_params.baseFactor) * _binStep * 10 *
                             pow10u(_params.baseFeePowerFactor);
    uint64_t variableFee = 0;
    if (_params.variableFeeControl > 0)
    {
      const u256 vfa = u256(static_cast<uint64_t>(v.volatilityAccumulator) * _binStep);
      const u256 vFee = u256(_params.variableFeeControl) * (vfa * vfa);
      variableFee = ((vFee + u256(99999999999ULL)) / u256(100000000000ULL)).w[0];
    }
    const uint64_t total = baseFee + variableFee;
    return total > kMaxFeeRate ? kMaxFeeRate : total;
  }

  static uint64_t pow10u(uint8_t p)
  {
    uint64_t r = 1;
    for (uint8_t k = 0; k < p; ++k)
    {
      r *= 10;
    }
    return r;
  }

  u256 computeFee(const u256& amount, uint64_t totalFee) const
  {
    return ceilDiv(amount * u256(totalFee), u256(kFeePrecision - totalFee));
  }
  u256 computeFeeFromAmount(const u256& amount, uint64_t totalFee) const
  {
    return ceilDiv(amount * u256(totalFee), u256(kFeePrecision));
  }

  void updateReferences(DlmmVolatility& v, int32_t active) const
  {
    const uint64_t elapsed = _ts - v.lastUpdateTimestamp;
    if (elapsed >= _params.filterPeriod)
    {
      v.indexReference = active;
      v.volatilityReference =
          elapsed < _params.decayPeriod
              ? static_cast<uint32_t>(static_cast<uint64_t>(v.volatilityAccumulator) *
                                      _params.reductionFactor / kBasisPointMax)
              : 0;
    }
  }

  void updateVolatilityAccumulator(DlmmVolatility& v, int32_t active) const
  {
    const uint64_t delta = static_cast<uint64_t>(std::abs(v.indexReference - active));
    const uint64_t va = static_cast<uint64_t>(v.volatilityReference) + delta * kBasisPointMax;
    v.volatilityAccumulator = static_cast<uint32_t>(std::min<uint64_t>(va, _params.maxVolatilityAccumulator));
  }

  // The bin walk: fill bins from the active one outward, each constant-sum at its
  // price with the per-bin dynamic fee (fee on input).
  u256 runSwap(bool swapForY, const u256& amountIn, int32_t& active, DlmmVolatility& v,
               bool /*mutate*/ = false) const
  {
    if (amountIn.isZero())
    {
      return u256(0);
    }
    updateReferences(v, active);
    u256 left = amountIn;
    u256 out(0);
    for (int guard = 0; guard < 8192; ++guard)
    {
      if (left.isZero() || active < _minId || active > _maxId)
      {
        break;
      }
      auto it = _bins.find(active);
      const u256 maxOut =
          it == _bins.end() ? u256(0) : (swapForY ? it->second.second : it->second.first);
      if (!maxOut.isZero())
      {
        updateVolatilityAccumulator(v, active);
        const uint64_t fee = totalFeeRate(v);
        const u256 price = binPrice(active);

        // fee on input: take the fee estimate off the input, fill with the net.
        const u256 feeEst = computeFeeFromAmount(left, fee);
        const u256 net = left - feeEst;
        const u256 maxIn = amountInAtBin(maxOut, price, swapForY);

        if (net >= maxIn)
        {
          // bin fully consumed; the fee is on the amount actually used.
          const u256 binFee = computeFee(maxIn, fee);
          out = out + maxOut;
          left = left - (maxIn + binFee);
        }
        else
        {
          // partial fill; the whole remaining input is spent here.
          out = out + amountOutAtBin(net, price, swapForY);
          left = u256(0);
        }
      }
      if (!left.isZero())
      {
        active += swapForY ? -1 : 1;
      }
    }
    return out;
  }

  int32_t _activeId;
  uint16_t _binStep;
  DlmmFeeParams _params;
  DlmmVolatility _vol;
  uint64_t _ts;
  std::map<int32_t, std::pair<u256, u256>> _bins;
  int32_t _minId{0};
  int32_t _maxId{0};
  mutable std::vector<u256> _bal;
};

}  // namespace flox
