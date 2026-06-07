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

// Raydium CLMM pool on Solana (concentrated liquidity), exact in integer math.
// Like Orca it is the Uniswap v3 swap math at a Q64.64 sqrt price, so it is a thin
// parameterization of ConcentratedLiquidityCurve, not a new transcription. The
// program's get_delta_amount_0_unsigned already uses the nested rounding the v3
// core does, and the next-sqrt-price and fee match; only the fixed-point unit and
// the maximum sqrt price differ from v3 (the maximum differs from Orca's too,
// since Raydium's tick math rounds the bound slightly differently). Ticks are
// ClTick with the sqrt price in Q64.64.
//
// Models the standard fee-on-input pool. The fee-on-output path (used for
// transfer-fee mints) and Token-2022 transfer fees are a separate boundary
// concern (W21.T010), not part of this curve.
class RaydiumClmmCurve : public ConcentratedLiquidityCurve
{
 public:
  RaydiumClmmCurve(u256 sqrtPriceX64, u256 liquidity, uint32_t feeRate, std::vector<ClTick> ticks)
      : ConcentratedLiquidityCurve(sqrtPriceX64, liquidity, feeRate, std::move(ticks), q64(),
                                   minSqrt(), maxSqrt())
  {
  }

  uint32_t feeRate() const { return feePips(); }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<RaydiumClmmCurve>(*this);
  }

 private:
  static u256 q64() { return u256::fromDec("18446744073709551616"); }  // 2^64
  static u256 minSqrt() { return u256::fromDec("4295048016"); }
  static u256 maxSqrt() { return u256::fromDec("79226673521066979257578248091"); }
};

}  // namespace flox
