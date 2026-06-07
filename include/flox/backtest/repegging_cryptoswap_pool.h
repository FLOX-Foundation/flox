/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/cryptoswap_curve.h"
#include "flox/util/int/u256.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace flox
{

// The full Curve V2 tricrypto pool: a CryptoswapCurve whose price scale moves.
// applySwap runs the contract's tweak_price after the trade -- the EMA price
// oracle (via halfpow), the xcp_profit / virtual_price update, and the gated
// step of price_scale toward the oracle with the accept-or-revert check -- all
// in the contract's integer arithmetic. The pricing surface is the exact
// CryptoswapCurve (get_dy 0 wei); this adds the state evolution across swaps.
//
// The EMA is time-based on chain (block.timestamp). The INTokenCurve interface
// carries no time, so each applySwap advances an internal clock by dtPerSwap; a
// backtest sets dtPerSwap to the spacing it wants between trades.
class RepeggingCryptoswapPool : public CryptoswapCurve
{
 public:
  RepeggingCryptoswapPool(std::vector<u256> balances, std::vector<u256> precisions,
                          std::vector<u256> priceScale, uint64_t A, u256 gamma, u256 midFee,
                          u256 outFee, u256 feeGamma, std::vector<u256> priceOracle,
                          std::vector<u256> lastPrices, u256 xcpProfit, u256 virtualPrice, u256 D,
                          u256 totalSupply, u256 maHalfTime, u256 allowedExtraProfit,
                          u256 adjustmentStep, bool notAdjusted, u256 dtPerSwap)
      : CryptoswapCurve(std::move(balances), std::move(precisions), std::move(priceScale), A, gamma,
                        midFee, outFee, feeGamma),
        _oracle(std::move(priceOracle)),
        _lastPrices(std::move(lastPrices)),
        _xcpProfit(xcpProfit),
        _virtualPrice(virtualPrice),
        _D(D),
        _totalSupply(totalSupply),
        _maHalfTime(maHalfTime),
        _allowedExtraProfit(allowedExtraProfit),
        _adjustmentStep(adjustmentStep),
        _notAdjusted(notAdjusted),
        _dt(dtPerSwap)
  {
  }

  const std::vector<u256>& priceOracle() const { return _oracle; }
  u256 xcpProfit() const { return _xcpProfit; }
  u256 virtualPrice() const { return _virtualPrice; }

  u256 applySwap(std::size_t i, std::size_t j, const u256& dx) override
  {
    const u256 out = amountOut(i, j, dx);
    _b[i] = _b[i] + dx;
    _b[j] = _b[j] - out;

    // p and ix for tweak_price, from the contract's exchange().
    const u256 dxp = dx * _prec[i];
    const u256 dyp = out * _prec[j];
    u256 p(0);
    std::size_t ix = j;
    if (i != 0 && j != 0)
    {
      p = _lastPrices[i - 1] * dxp / dyp;
    }
    else if (i == 0)
    {
      p = dxp * P() / dyp;
    }
    else
    {
      p = dyp * P() / dxp;
      ix = i;
    }
    tweakPrice(xpOf(_b), ix, p);
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<RepeggingCryptoswapPool>(*this);
  }

 private:
  // 1e18 * 0.5^(power/1e18), transcribed from CurveCryptoMath3.halfpow.
  static u256 halfpow(const u256& power, const u256& precision)
  {
    const u256 one = P();
    const u256 ip = power / one;
    const u256 op = power - ip * one;
    if (ip > u256(59))
    {
      return u256(0);
    }
    u256 twoIp(1);
    for (u256 k(0); k < ip; k = k + u256(1))
    {
      twoIp = twoIp * u256(2);
    }
    u256 result = one / twoIp;
    if (op.isZero())
    {
      return result;
    }
    u256 term = one;
    const u256 x = u256(5) * u256::pow10(17);
    u256 S = one;
    bool neg = false;
    for (int i = 1; i < 256; ++i)
    {
      const u256 K = u256(i) * one;
      u256 c = K - one;
      if (op > c)
      {
        c = op - c;
        neg = !neg;
      }
      else
      {
        c = c - op;
      }
      term = term * (c * x / one) / K;
      S = neg ? (S - term) : (S + term);
      if (term < precision)
      {
        return result * S / one;
      }
    }
    return result * S / one;
  }

  // sqrt(x * 1e18), transcribed from CurveCryptoMath3.sqrt_int.
  static u256 sqrtInt(const u256& x)
  {
    if (x.isZero())
    {
      return u256(0);
    }
    u256 z = (x + P()) / u256(2);
    u256 y = x;
    for (int i = 0; i < 256; ++i)
    {
      if (z == y)
      {
        return y;
      }
      y = z;
      z = (x * P() / z + z) / u256(2);
    }
    return y;
  }

  // CurveCryptoSwap.tweak_price for an exchange (p > 0).
  void tweakPrice(const std::vector<u256>& xpAfter, std::size_t ix, const u256& p)
  {
    const std::size_t n = _b.size();
    const u256 one = P();

    // EMA oracle update over the elapsed dt.
    if (!_dt.isZero())
    {
      const u256 alpha = halfpow(_dt * one / _maHalfTime, u256::pow10(10));
      for (std::size_t k = 0; k + 1 < n; ++k)
      {
        _oracle[k] = (_lastPrices[k] * (one - alpha) + _oracle[k] * alpha) / one;
      }
    }

    const u256 dUnadjusted = newtonD(xpAfter);

    // Save the last price.
    if (ix > 0)
    {
      _lastPrices[ix - 1] = p;
    }
    else
    {
      for (std::size_t k = 0; k + 1 < n; ++k)
      {
        _lastPrices[k] = _lastPrices[k] * one / p;
      }
    }

    const u256 oldXcpProfit = _xcpProfit;
    const u256 oldVirtualPrice = _virtualPrice;

    // Profit numbers at the current (unadjusted) scale.
    std::vector<u256> xp(n);
    xp[0] = dUnadjusted / u256(n);
    for (std::size_t k = 0; k + 1 < n; ++k)
    {
      xp[k + 1] = dUnadjusted * one / (u256(n) * _scale[k]);
    }
    u256 xcpProfit = one;
    u256 virtualPrice = one;
    if (!oldVirtualPrice.isZero())
    {
      const u256 xcp = geometricMean(xp, true);
      virtualPrice = one * xcp / _totalSupply;
      xcpProfit = oldXcpProfit * virtualPrice / oldVirtualPrice;
    }
    _xcpProfit = xcpProfit;

    bool needs = _notAdjusted;
    if (!needs && virtualPrice * u256(2) > one + xcpProfit + u256(2) * _allowedExtraProfit)
    {
      needs = true;
      _notAdjusted = true;
    }

    if (needs)
    {
      u256 norm(0);
      for (std::size_t k = 0; k + 1 < n; ++k)
      {
        u256 ratio = _oracle[k] * one / _scale[k];
        ratio = (ratio > one) ? (ratio - one) : (one - ratio);
        norm = norm + ratio * ratio;
      }
      if (norm > _adjustmentStep * _adjustmentStep && !oldVirtualPrice.isZero())
      {
        norm = sqrtInt(norm / one);
        std::vector<u256> pNew(n - 1);
        for (std::size_t k = 0; k + 1 < n; ++k)
        {
          pNew[k] =
              (_scale[k] * (norm - _adjustmentStep) + _adjustmentStep * _oracle[k]) / norm;
        }
        std::vector<u256> xpN = xpAfter;
        for (std::size_t k = 0; k + 1 < n; ++k)
        {
          xpN[k + 1] = xpAfter[k + 1] * pNew[k] / _scale[k];
        }
        const u256 d2 = newtonD(xpN);
        std::vector<u256> xp2(n);
        xp2[0] = d2 / u256(n);
        for (std::size_t k = 0; k + 1 < n; ++k)
        {
          xp2[k + 1] = d2 * one / (u256(n) * pNew[k]);
        }
        const u256 vp2 = one * geometricMean(xp2, true) / _totalSupply;
        if (vp2 > one && u256(2) * vp2 > one + xcpProfit)
        {
          _scale = pNew;
          _D = d2;
          _virtualPrice = vp2;
          return;  // adjustment accepted
        }
        _notAdjusted = false;
      }
    }
    _D = dUnadjusted;
    _virtualPrice = virtualPrice;
  }

  std::vector<u256> _oracle;
  std::vector<u256> _lastPrices;
  u256 _xcpProfit;
  u256 _virtualPrice;
  u256 _D;
  u256 _totalSupply;
  u256 _maHalfTime;
  u256 _allowedExtraProfit;
  u256 _adjustmentStep;
  bool _notAdjusted;
  u256 _dt;
};

}  // namespace flox
