/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/concentrated_liquidity_curve.h"
#include "flox/util/int/u256.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace flox
{

// Orca Whirlpool pool on Solana (concentrated liquidity), exact in integer math.
// It is the Uniswap v3 swap math at a Q64.64 sqrt price instead of Q64.96. The
// Whirlpool program writes get_amount_delta_a as a single division over the full
// 256-bit numerator, where v3 nests two divisions; those are equal for all inputs
// (ceil(ceil(a/b)/c) == ceil(a/(bc)) and the floor analogue), so the swap reduces
// to the same ConcentratedLiquidityCurve, only the fixed-point unit and the
// min/max sqrt price differ. Ticks are ClTick with the sqrt price in Q64.64.
//
// Models a static-fee Whirlpool, the common kind; the opt-in adaptive-fee
// extension (the program's volatility-tracking FeeRateManager) is a separate
// feature, not modelled here, as the v3 curve does not model v4 hooks.
class OrcaWhirlpoolCurve : public ConcentratedLiquidityCurve
{
 public:
  OrcaWhirlpoolCurve(u256 sqrtPriceX64, u256 liquidity, uint32_t feeRate, std::vector<ClTick> ticks)
      : ConcentratedLiquidityCurve(sqrtPriceX64, liquidity, feeRate, std::move(ticks), q64(),
                                   minSqrt(), maxSqrt())
  {
  }

  uint32_t feeRate() const { return feePips(); }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<OrcaWhirlpoolCurve>(*this);
  }

 private:
  static u256 q64() { return u256::fromDec("18446744073709551616"); }  // 2^64
  static u256 minSqrt() { return u256::fromDec("4295048016"); }
  static u256 maxSqrt() { return u256::fromDec("79226673515401279992447579055"); }
};

}  // namespace flox
