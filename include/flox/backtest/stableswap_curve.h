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

// Curve StableSwap pool (3pool style), exact in native-wei integer math,
// reproducing get_dy to the wei. Replicates the contract's integer algorithm:
// the balances are normalized to a common scale by RATES (3pool: DAI * 1e18,
// USDC / USDT * 1e30, against PRECISION 1e18), the invariant D and the output
// balance y come from integer Newton with the contract's `abs(delta) <= 1`
// termination, and the fee is taken on the output after the defensive `-1`.
//
// Ann is A * N (the original StableSwap convention, confirmed 0-wei against the
// live 3pool), not A * N^N. Parameterized by balances, rates, A, and fee, so one
// class covers 3pool and other plain stableswap pools; n is balances.size().
class StableSwapCurve : public INTokenCurve
{
 public:
  // balances and rates are per coin (native wei). A is the amplification the
  // contract's A() returns. fee is the raw contract fee over FEE_DENOMINATOR
  // (1e10): 3pool's 1500000 is 0.015%.
  StableSwapCurve(std::vector<u256> balances, std::vector<u256> rates, uint64_t A, u256 fee)
      : _b(std::move(balances)), _rates(std::move(rates)), _A(A), _fee(fee)
  {
  }

  uint64_t amp() const { return _A; }
  const std::vector<u256>& rates() const { return _rates; }

  std::size_t tokenCount() const override { return _b.size(); }
  const std::vector<u256>& balances() const override { return _b; }

  u256 amountOut(std::size_t i, std::size_t j, const u256& dx) const override
  {
    if (dx.isZero())
    {
      return u256(0);
    }
    const std::vector<u256> xp = xpMem();
    const u256 x = xp[i] + mulDiv(dx, _rates[i], prec());
    const u256 D = getD(xp);
    const u256 y = getY(i, j, x, xp, D);
    if (xp[j] <= y + u256(1))
    {
      return u256(0);
    }
    const u256 dy = mulDiv(xp[j] - y - u256(1), prec(), _rates[j]);
    const u256 fee = mulDiv(_fee, dy, feeDenom());
    return dy - fee;
  }

  u256 applySwap(std::size_t i, std::size_t j, const u256& dx) override
  {
    const u256 out = amountOut(i, j, dx);
    _b[i] = _b[i] + dx;
    _b[j] = _b[j] - out;
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<StableSwapCurve>(*this);
  }

 private:
  static u256 prec() { return u256::pow10(18); }
  static u256 feeDenom() { return u256::pow10(10); }
  u256 ann() const { return u256(_A) * u256(_b.size()); }

  std::vector<u256> xpMem() const
  {
    std::vector<u256> xp(_b.size());
    for (std::size_t k = 0; k < _b.size(); ++k)
    {
      xp[k] = mulDiv(_rates[k], _b[k], prec());
    }
    return xp;
  }

  // Invariant D for the normalized balances, integer Newton, terminate at <= 1.
  u256 getD(const std::vector<u256>& xp) const
  {
    const u256 n(xp.size());
    u256 S(0);
    for (const u256& x : xp)
    {
      S = S + x;
    }
    if (S.isZero())
    {
      return u256(0);
    }
    const u256 Ann = ann();
    u256 D = S;
    for (int it = 0; it < 255; ++it)
    {
      u256 dP = D;
      for (const u256& x : xp)
      {
        dP = mulDiv(dP, D, x * n);
      }
      const u256 Dprev = D;
      D = mulDiv(Ann * S + dP * n, D, (Ann - u256(1)) * D + (n + u256(1)) * dP);
      if (diffLE1(D, Dprev))
      {
        break;
      }
    }
    return D;
  }

  // Balance of coin j for the new balance x of coin i, integer Newton on
  // y = (y^2 + c) / (2y + b - D), terminate at <= 1.
  u256 getY(std::size_t i, std::size_t j, const u256& x, const std::vector<u256>& xp,
            const u256& D) const
  {
    const u256 n(xp.size());
    const u256 Ann = ann();
    u256 c = D;
    u256 S_(0);
    for (std::size_t k = 0; k < xp.size(); ++k)
    {
      if (k == j)
      {
        continue;
      }
      const u256& xk = (k == i) ? x : xp[k];
      S_ = S_ + xk;
      c = mulDiv(c, D, xk * n);
    }
    c = mulDiv(c, D, Ann * n);
    const u256 b = S_ + D / Ann;
    u256 y = D;
    for (int it = 0; it < 255; ++it)
    {
      const u256 yprev = y;
      y = mulDiv(y, y, u256(1));  // y*y exactly
      y = (y + c) / (u256(2) * yprev + b - D);
      if (diffLE1(y, yprev))
      {
        break;
      }
    }
    return y;
  }

  static bool diffLE1(const u256& a, const u256& b)
  {
    return (a > b) ? (a - b <= u256(1)) : (b - a <= u256(1));
  }

  std::vector<u256> _b;
  std::vector<u256> _rates;
  uint64_t _A;
  u256 _fee;
};

}  // namespace flox
