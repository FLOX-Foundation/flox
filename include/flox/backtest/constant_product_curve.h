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

// Constant-product pool (Uniswap V2 / SushiSwap / PancakeSwap and forks), exact
// in native-wei integer math. A two-token pool, so an n = 2 INTokenCurve. The
// fee is the contract's integer fee, given as a numerator/denominator so one
// class covers the forks: Uniswap V2 is 997/1000 (0.30%), PancakeSwap V2 is
// 9975/10000 (0.25%). The swap reproduces getAmountsOut to the wei:
//
//   inWithFee = amountIn * feeNum
//   amountOut = inWithFee * reserveOut / (reserveIn * feeDen + inWithFee)   [floor]
class ConstantProductCurve : public INTokenCurve
{
 public:
  ConstantProductCurve(u256 reserve0, u256 reserve1, uint64_t feeNum, uint64_t feeDen)
      : _b{reserve0, reserve1}, _feeNum(feeNum), _feeDen(feeDen)
  {
  }

  uint64_t feeNum() const { return _feeNum; }
  uint64_t feeDen() const { return _feeDen; }

  std::size_t tokenCount() const override { return 2; }
  const std::vector<u256>& balances() const override { return _b; }

  u256 amountOut(std::size_t i, std::size_t j, const u256& amountIn) const override
  {
    if (amountIn.isZero() || _b[i].isZero() || _b[j].isZero())
    {
      return u256(0);
    }
    const u256 inWithFee = amountIn * u256(_feeNum);
    const u256 denom = _b[i] * u256(_feeDen) + inWithFee;
    return mulDiv(inWithFee, _b[j], denom);  // floor, full 512-bit intermediate
  }

  u256 applySwap(std::size_t i, std::size_t j, const u256& amountIn) override
  {
    const u256 out = amountOut(i, j, amountIn);
    _b[i] = _b[i] + amountIn;
    _b[j] = _b[j] - out;
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<ConstantProductCurve>(*this);
  }

 private:
  std::vector<u256> _b;
  uint64_t _feeNum;
  uint64_t _feeDen;
};

}  // namespace flox
