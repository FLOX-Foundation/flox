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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace flox
{

// Raydium constant-product pool (CPMM) on Solana, exact in native-lamport integer
// math, transcribed from the program (CurveCalculator::swap_base_input). The core
// is the same constant product as the EVM forks -- out = net * outVault /
// (inVault + net), floored -- but the fee handling is Raydium's.
//
// The two balances are the swappable reserves: each vault's token balance minus
// its accumulated (protocol + fund + creator) fees, which is what the program
// feeds the curve. They evolve by the program's SwapResult: the input reserve
// grows by the input net of fees, the output reserve falls by the full swapped
// amount.
//
// Fees are rates over a 1e6 denominator (2500 = 0.25%), removed as a ceil-div.
// The creator fee has two on-chain modes:
//   - creatorFeeOnInput: the trade and creator rates are summed and removed from
//     the input in one ceil-div; the user receives the full swapped output.
//   - creator fee on output: only the trade rate is removed from the input; after
//     the swap a ceil-div creator fee is removed from the output, so the user
//     receives less than the pool releases. The reserve still falls by the full
//     swapped amount; the creator fee is set aside.
// A pool with no creator fee (the common case) is identical in both modes.
class RaydiumCpCurve : public INTokenCurve
{
 public:
  RaydiumCpCurve(u256 vault0, u256 vault1, uint64_t tradeFeeRate, uint64_t creatorFeeRate = 0,
                 bool creatorFeeOnInput = true)
      : _b{vault0, vault1},
        _tradeFeeRate(tradeFeeRate),
        _creatorFeeRate(creatorFeeRate),
        _creatorFeeOnInput(creatorFeeOnInput)
  {
  }

  uint64_t tradeFeeRate() const { return _tradeFeeRate; }
  uint64_t creatorFeeRate() const { return _creatorFeeRate; }
  bool creatorFeeOnInput() const { return _creatorFeeOnInput; }

  std::size_t tokenCount() const override { return 2; }
  const std::vector<u256>& balances() const override { return _b; }

  // What the user receives.
  u256 amountOut(std::size_t i, std::size_t j, const u256& amountIn) const override
  {
    return swap(i, j, amountIn).userOut;
  }

  u256 applySwap(std::size_t i, std::size_t j, const u256& amountIn) override
  {
    const Swap s = swap(i, j, amountIn);
    _b[i] = _b[i] + s.netIn;    // input reserve grows by the input net of fees
    _b[j] = _b[j] - s.poolOut;  // output reserve falls by the full swapped amount
    return s.userOut;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<RaydiumCpCurve>(*this);
  }

 private:
  struct Swap
  {
    u256 netIn;    // input added to the input reserve
    u256 poolOut;  // amount the output reserve releases
    u256 userOut;  // amount the user receives (poolOut minus any on-output creator fee)
  };

  Swap swap(std::size_t i, std::size_t j, const u256& amountIn) const
  {
    if (amountIn.isZero() || _b[i].isZero() || _b[j].isZero())
    {
      return {u256(0), u256(0), u256(0)};
    }

    const uint64_t inputRate = _creatorFeeOnInput ? _tradeFeeRate + _creatorFeeRate : _tradeFeeRate;
    const u256 inputFee = ceilFee(amountIn, inputRate);
    if (inputFee >= amountIn)
    {
      return {u256(0), u256(0), u256(0)};
    }
    const u256 netIn = amountIn - inputFee;

    const u256 poolOut = netIn * _b[j] / (_b[i] + netIn);  // floor, the constant product

    u256 userOut = poolOut;
    if (!_creatorFeeOnInput)
    {
      userOut = poolOut - ceilFee(poolOut, _creatorFeeRate);  // creator fee taken on output
    }
    return {netIn, poolOut, userOut};
  }

  // ceil(amount * rate / 1e6), the program's ceil_div.
  static u256 ceilFee(const u256& amount, uint64_t rate)
  {
    if (rate == 0)
    {
      return u256(0);
    }
    const u256 denom = u256::pow10(6);
    return (amount * u256(rate) + denom - u256(1)) / denom;
  }

  std::vector<u256> _b;
  uint64_t _tradeFeeRate;
  uint64_t _creatorFeeRate;
  bool _creatorFeeOnInput;
};

}  // namespace flox
