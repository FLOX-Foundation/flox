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
#include "flox/util/int/i256.h"
#include "flox/util/int/u256.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace flox
{

// An initialized tick: the sqrt price (Q64.96) at the tick boundary and the
// liquidity that becomes active or inactive when crossing it upward.
struct ClTick
{
  u256 sqrtRatioX96;
  i256 liquidityNet;
};

// Concentrated-liquidity pool (Uniswap v3 style), exact in integer math. The
// swap math is a direct transcription of the v3 SwapMath / SqrtPriceMath /
// FullMath, so the Q64.96 sqrt-price steps and their up / down rounding match
// the contract. A swap walks the initialized ticks: within a range it is a
// single computeSwapStep on the active liquidity, and crossing an initialized
// tick updates the liquidity by its liquidityNet. token0 is index 0, token1 is
// index 1, so a token0-in swap moves the price down. Reproduces QuoterV2 to the
// wei.
class ConcentratedLiquidityCurve : public INTokenCurve
{
 public:
  // ticks must be sorted ascending by sqrtRatioX96.
  ConcentratedLiquidityCurve(u256 sqrtPriceX96, u256 liquidity, uint32_t feePips,
                             std::vector<ClTick> ticks)
      : _sqrtP(sqrtPriceX96), _L(liquidity), _fee(feePips), _ticks(std::move(ticks))
  {
  }

  u256 sqrtPriceX96() const { return _sqrtP; }
  u256 liquidity() const { return _L; }
  uint32_t feePips() const { return _fee; }

  std::size_t tokenCount() const override { return 2; }

  // Virtual reserves at the current price, so the interface has a composition to
  // report. amount0 = L * Q96 / sqrtP, amount1 = L * sqrtP / Q96.
  const std::vector<u256>& balances() const override
  {
    _bal.resize(2);
    _bal[0] = _L.isZero() ? u256(0) : mulDiv(_L, q96(), _sqrtP);
    _bal[1] = mulDiv(_L, _sqrtP, q96());
    return _bal;
  }

  u256 amountOut(std::size_t i, std::size_t j, const u256& dx) const override
  {
    (void)j;
    u256 sqrt = _sqrtP;
    u256 L = _L;
    return runSwap(i == 0, dx, sqrt, L);
  }

  u256 applySwap(std::size_t i, std::size_t j, const u256& dx) override
  {
    (void)j;
    u256 sqrt = _sqrtP;
    u256 L = _L;
    const u256 out = runSwap(i == 0, dx, sqrt, L);
    _sqrtP = sqrt;
    _L = L;
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<ConcentratedLiquidityCurve>(*this);
  }

 private:
  static u256 q96() { return u256::fromDec("79228162514264337593543950336"); }  // 2^96
  static u256 minSqrt() { return u256::fromDec("4295128739"); }
  static u256 maxSqrt()
  {
    return u256::fromDec("1461446703485210103287273052203988822378723970342");
  }

  // The v3 swap loop. Walks ticks, mutating sqrt and L (by reference); returns
  // the total output.
  u256 runSwap(bool zeroForOne, const u256& dx, u256& sqrt, u256& L) const
  {
    if (dx.isZero())
    {
      return u256(0);
    }
    u256 rem = dx;
    u256 out(0);
    const u256 limit = zeroForOne ? minSqrt() + u256(1) : maxSqrt() - u256(1);
    for (int guard = 0; guard < 4096; ++guard)
    {
      if (rem.isZero() || sqrt == limit || L.isZero())
      {
        break;
      }
      bool hasTick = false;
      u256 tickSqrt(0);
      i256 net(0);
      findNextTick(sqrt, zeroForOne, hasTick, tickSqrt, net);
      u256 target = limit;
      if (hasTick)
      {
        target = zeroForOne ? (tickSqrt > limit ? tickSqrt : limit)
                            : (tickSqrt < limit ? tickSqrt : limit);
      }
      u256 amountIn(0), amountOutStep(0), feeAmount(0);
      const u256 sqrtNext =
          computeStep(sqrt, target, L, rem, zeroForOne, _fee, amountIn, amountOutStep, feeAmount);
      rem = rem - (amountIn + feeAmount);
      out = out + amountOutStep;
      if (hasTick && sqrtNext == tickSqrt)
      {
        const i256 dL = zeroForOne ? -net : net;
        L = dL.neg ? (L - dL.mag) : (L + dL.mag);
      }
      sqrt = sqrtNext;
    }
    return out;
  }

  void findNextTick(const u256& sqrt, bool zeroForOne, bool& has, u256& tickSqrt, i256& net) const
  {
    has = false;
    if (zeroForOne)
    {
      for (std::size_t k = _ticks.size(); k-- > 0;)
      {
        if (_ticks[k].sqrtRatioX96 < sqrt)
        {
          has = true;
          tickSqrt = _ticks[k].sqrtRatioX96;
          net = _ticks[k].liquidityNet;
          return;
        }
      }
    }
    else
    {
      for (std::size_t k = 0; k < _ticks.size(); ++k)
      {
        if (_ticks[k].sqrtRatioX96 > sqrt)
        {
          has = true;
          tickSqrt = _ticks[k].sqrtRatioX96;
          net = _ticks[k].liquidityNet;
          return;
        }
      }
    }
  }

  static u256 divRoundingUp(const u256& a, const u256& b)
  {
    return a.isZero() ? u256(0) : (a - u256(1)) / b + u256(1);
  }

  static u256 getAmount0Delta(u256 a, u256 b, const u256& L, bool roundUp)
  {
    if (a > b)
    {
      std::swap(a, b);
    }
    const u256 n1 = L * q96();
    const u256 n2 = b - a;
    return roundUp ? divRoundingUp(mulDivUp(n1, n2, b), a) : mulDiv(n1, n2, b) / a;
  }
  static u256 getAmount1Delta(u256 a, u256 b, const u256& L, bool roundUp)
  {
    if (a > b)
    {
      std::swap(a, b);
    }
    return roundUp ? mulDivUp(L, b - a, q96()) : mulDiv(L, b - a, q96());
  }

  static u256 nextFromAmount0Up(const u256& sp, const u256& L, const u256& amount)
  {
    if (amount.isZero())
    {
      return sp;
    }
    const u256 n1 = L * q96();
    const u256 prod = amount * sp;
    const u256 den = n1 + prod;
    return mulDivUp(n1, sp, den);
  }
  static u256 nextFromAmount1Down(const u256& sp, const u256& L, const u256& amount)
  {
    return sp + (amount * q96()) / L;
  }
  static u256 nextFromInput(const u256& sp, const u256& L, const u256& amountIn, bool zeroForOne)
  {
    return zeroForOne ? nextFromAmount0Up(sp, L, amountIn) : nextFromAmount1Down(sp, L, amountIn);
  }

  // v3 SwapMath.computeSwapStep, exact-input branch.
  static u256 computeStep(const u256& sc, const u256& st, const u256& L, const u256& amtRemaining,
                          bool zeroForOne, uint32_t feePips, u256& amountIn, u256& amountOut,
                          u256& feeAmount)
  {
    const u256 feeNum = u256(1000000 - feePips);
    const u256 amountRemainingLessFee = mulDiv(amtRemaining, feeNum, u256(1000000));
    amountIn = zeroForOne ? getAmount0Delta(st, sc, L, true) : getAmount1Delta(sc, st, L, true);
    u256 sn;
    if (amountRemainingLessFee >= amountIn)
    {
      sn = st;
    }
    else
    {
      sn = nextFromInput(sc, L, amountRemainingLessFee, zeroForOne);
    }
    const bool max = (st == sn);
    if (zeroForOne)
    {
      amountIn = max ? amountIn : getAmount0Delta(sn, sc, L, true);
      amountOut = getAmount1Delta(sn, sc, L, false);
    }
    else
    {
      amountIn = max ? amountIn : getAmount1Delta(sc, sn, L, true);
      amountOut = getAmount0Delta(sc, sn, L, false);
    }
    if (sn != st)
    {
      feeAmount = amtRemaining - amountIn;
    }
    else
    {
      feeAmount = mulDivUp(amountIn, u256(feePips), feeNum);
    }
    return sn;
  }

  u256 _sqrtP;
  u256 _L;
  uint32_t _fee;
  std::vector<ClTick> _ticks;
  mutable std::vector<u256> _bal;
};

}  // namespace flox
