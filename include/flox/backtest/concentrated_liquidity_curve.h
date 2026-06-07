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
#include <utility>
#include <vector>

namespace flox
{

// An initialized tick: the sqrt price at the tick boundary (in the pool's own
// fixed point) and the liquidity that becomes active or inactive when crossing it
// upward.
struct ClTick
{
  u256 sqrtRatio;
  i256 liquidityNet;
};

// Concentrated-liquidity pool, exact in integer math. A direct transcription of
// the Uniswap v3 SwapMath / SqrtPriceMath / FullMath: a swap walks the
// initialized ticks, within a range it is a single computeStep on the active
// liquidity, and crossing an initialized tick updates the liquidity by its
// liquidityNet. token0 is index 0, token1 is index 1, so a token0-in swap moves
// the price down. The default parameters are Uniswap v3 (Q64.96 sqrt price); the
// protected constructor lets a venue with the same math at a different fixed
// point reuse it (see OrcaWhirlpoolCurve, RaydiumClmmCurve). Reproduces QuoterV2
// to the wei.
class ConcentratedLiquidityCurve : public INTokenCurve
{
 public:
  // Uniswap v3: ticks sorted ascending by sqrtRatio, Q64.96.
  ConcentratedLiquidityCurve(u256 sqrtRatio, u256 liquidity, uint32_t feePips, std::vector<ClTick> ticks)
      : _sqrtP(sqrtRatio),
        _L(liquidity),
        _fee(feePips),
        _ticks(std::move(ticks)),
        _q(u256::fromDec("79228162514264337593543950336")),  // 2^96
        _minSqrt(u256::fromDec("4295128739")),
        _maxSqrt(u256::fromDec("1461446703485210103287273052203988822378723970342"))
  {
  }

  u256 sqrtPrice() const { return _sqrtP; }
  u256 liquidity() const { return _L; }
  uint32_t feePips() const { return _fee; }

  std::size_t tokenCount() const override { return 2; }

  // Virtual reserves at the current price: amount0 = L * Q / sqrtP,
  // amount1 = L * sqrtP / Q, with Q the pool's fixed-point unit.
  const std::vector<u256>& balances() const override
  {
    _bal.resize(2);
    _bal[0] = _L.isZero() ? u256(0) : mulDiv(_L, _q, _sqrtP);
    _bal[1] = mulDiv(_L, _sqrtP, _q);
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

  // Exact-output swap: the input (native wei) required to take exactly dy of the
  // out-token j, without moving the pool. This is the other v3 SwapMath branch (the
  // contract rounds the price the opposite way from exact-input), the one a
  // router-style exact-output swap takes.
  u256 amountInForOutput(std::size_t i, std::size_t j, const u256& dy) const
  {
    (void)j;
    u256 sqrt = _sqrtP;
    u256 L = _L;
    return runSwapExactOut(i == 0, dy, sqrt, L);
  }

  // Apply an exact-output swap: move the pool to take exactly dy of the out-token and
  // return the input spent.
  u256 applySwapForOutput(std::size_t i, std::size_t j, const u256& dy)
  {
    (void)j;
    u256 sqrt = _sqrtP;
    u256 L = _L;
    const u256 in = runSwapExactOut(i == 0, dy, sqrt, L);
    _sqrtP = sqrt;
    _L = L;
    return in;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<ConcentratedLiquidityCurve>(*this);
  }

 protected:
  // For a venue with the same v3 swap math at a different fixed point and price
  // bounds. The delta and next-price formulas are identical; only the unit Q and
  // the min/max sqrt price differ.
  ConcentratedLiquidityCurve(u256 sqrtRatio, u256 liquidity, uint32_t feePips,
                             std::vector<ClTick> ticks, u256 q, u256 minSqrt, u256 maxSqrt)
      : _sqrtP(sqrtRatio),
        _L(liquidity),
        _fee(feePips),
        _ticks(std::move(ticks)),
        _q(q),
        _minSqrt(minSqrt),
        _maxSqrt(maxSqrt)
  {
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
    const u256 limit = zeroForOne ? _minSqrt + u256(1) : _maxSqrt - u256(1);
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
        if (_ticks[k].sqrtRatio < sqrt)
        {
          has = true;
          tickSqrt = _ticks[k].sqrtRatio;
          net = _ticks[k].liquidityNet;
          return;
        }
      }
    }
    else
    {
      for (std::size_t k = 0; k < _ticks.size(); ++k)
      {
        if (_ticks[k].sqrtRatio > sqrt)
        {
          has = true;
          tickSqrt = _ticks[k].sqrtRatio;
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

  u256 getAmount0Delta(u256 a, u256 b, const u256& L, bool roundUp) const
  {
    if (a > b)
    {
      std::swap(a, b);
    }
    const u256 n1 = L * _q;
    const u256 n2 = b - a;
    return roundUp ? divRoundingUp(mulDivUp(n1, n2, b), a) : mulDiv(n1, n2, b) / a;
  }
  u256 getAmount1Delta(u256 a, u256 b, const u256& L, bool roundUp) const
  {
    if (a > b)
    {
      std::swap(a, b);
    }
    return roundUp ? mulDivUp(L, b - a, _q) : mulDiv(L, b - a, _q);
  }

  u256 nextFromAmount0Up(const u256& sp, const u256& L, const u256& amount) const
  {
    if (amount.isZero())
    {
      return sp;
    }
    const u256 n1 = L * _q;
    const u256 prod = amount * sp;
    const u256 den = n1 + prod;
    return mulDivUp(n1, sp, den);
  }
  u256 nextFromAmount1Down(const u256& sp, const u256& L, const u256& amount) const
  {
    return sp + (amount * _q) / L;
  }
  u256 nextFromInput(const u256& sp, const u256& L, const u256& amountIn, bool zeroForOne) const
  {
    return zeroForOne ? nextFromAmount0Up(sp, L, amountIn) : nextFromAmount1Down(sp, L, amountIn);
  }

  // v3 SwapMath.computeSwapStep, exact-input branch. feePips is the fee for this
  // step (the fixed pool fee for a static pool, the per-step rate for an adaptive
  // one).
  u256 computeStep(const u256& sc, const u256& st, const u256& L, const u256& amtRemaining,
                   bool zeroForOne, uint32_t feePips, u256& amountIn, u256& amountOut,
                   u256& feeAmount) const
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

  // v3 SqrtPriceMath.getNextSqrtPriceFromOutput, the price after removing `amount` of
  // the out-token. zeroForOne removes token1 (round the quotient up, subtract);
  // oneForZero removes token0 (round the quotient up via the reciprocal form).
  u256 nextFromAmount1RemoveDown(const u256& sp, const u256& L, const u256& amount) const
  {
    const u256 quotient = mulDivUp(amount, _q, L);
    return quotient >= sp ? _minSqrt : sp - quotient;  // unreachable in one step -> clamp
  }
  u256 nextFromAmount0RemoveUp(const u256& sp, const u256& L, const u256& amount) const
  {
    const u256 numerator1 = L * _q;
    const u256 product = amount * sp;
    if (product >= numerator1)
    {
      return _maxSqrt;  // unreachable in one step -> clamp
    }
    return mulDivUp(numerator1, sp, numerator1 - product);
  }
  u256 nextFromOutput(const u256& sp, const u256& L, const u256& amountOut, bool zeroForOne) const
  {
    return zeroForOne ? nextFromAmount1RemoveDown(sp, L, amountOut)
                      : nextFromAmount0RemoveUp(sp, L, amountOut);
  }

  // v3 SwapMath.computeSwapStep, exact-output branch: amtOutRemaining is the output
  // still wanted. The input is rounded up and the price rounded the opposite way from
  // the exact-input step, matching the contract.
  u256 computeStepOut(const u256& sc, const u256& st, const u256& L, const u256& amtOutRemaining,
                      bool zeroForOne, uint32_t feePips, u256& amountIn, u256& amountOut,
                      u256& feeAmount) const
  {
    const u256 maxToTarget =
        zeroForOne ? getAmount1Delta(st, sc, L, false) : getAmount0Delta(sc, st, L, false);
    u256 sn = (amtOutRemaining >= maxToTarget) ? st : nextFromOutput(sc, L, amtOutRemaining, zeroForOne);
    const bool max = (st == sn);
    if (zeroForOne)
    {
      amountIn = getAmount0Delta(sn, sc, L, true);
      amountOut = max ? maxToTarget : getAmount1Delta(sn, sc, L, false);
    }
    else
    {
      amountIn = getAmount1Delta(sc, sn, L, true);
      amountOut = max ? maxToTarget : getAmount0Delta(sc, sn, L, false);
    }
    if (amountOut > amtOutRemaining)
    {
      amountOut = amtOutRemaining;  // never release more than asked
    }
    const u256 feeNum = u256(1000000 - feePips);
    feeAmount = mulDivUp(amountIn, u256(feePips), feeNum);
    return sn;
  }

  // The v3 swap loop driven by a desired output. Walks ticks like runSwap, mutating
  // sqrt and L; returns the total input (incl. fee).
  u256 runSwapExactOut(bool zeroForOne, const u256& dy, u256& sqrt, u256& L) const
  {
    if (dy.isZero())
    {
      return u256(0);
    }
    u256 remOut = dy;
    u256 totalIn(0);
    const u256 limit = zeroForOne ? _minSqrt + u256(1) : _maxSqrt - u256(1);
    for (int guard = 0; guard < 4096; ++guard)
    {
      if (remOut.isZero() || sqrt == limit || L.isZero())
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
          computeStepOut(sqrt, target, L, remOut, zeroForOne, _fee, amountIn, amountOutStep, feeAmount);
      remOut = remOut - amountOutStep;
      totalIn = totalIn + amountIn + feeAmount;
      if (hasTick && sqrtNext == tickSqrt)
      {
        const i256 dL = zeroForOne ? -net : net;
        L = dL.neg ? (L - dL.mag) : (L + dL.mag);
      }
      sqrt = sqrtNext;
    }
    return totalIn;
  }

  u256 _sqrtP;
  u256 _L;
  uint32_t _fee;
  std::vector<ClTick> _ticks;
  u256 _q;
  u256 _minSqrt;
  u256 _maxSqrt;
  mutable std::vector<u256> _bal;
};

}  // namespace flox
