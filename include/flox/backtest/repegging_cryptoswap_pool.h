/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/cryptoswap_pool_n.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace flox
{

// The full Curve V2 tricrypto pool: a CryptoswapPoolN whose price scale moves.
// The static pool concentrates liquidity at a fixed price scale; a real
// cryptoswap pool re-centers that scale on the traded price, with no external
// oracle, when doing so pays for itself out of accumulated fees. Two extra
// pieces ride on top of the static curve:
//
//   - A dynamic fee that rises as the pool goes imbalanced (mid_fee when
//     balanced, toward out_fee when lopsided), blended by fee_gamma.
//   - Repegging: after each swap an internal EMA price oracle tracks the traded
//     price, and when the oracle has drifted far enough from the scale AND the
//     pool's virtual price shows enough profit to fund it, the scale steps
//     toward the oracle. The step is reverted if it would not leave the pool in
//     profit, so a rebalance never costs the LPs their fee income.
//
// Each applySwap advances the oracle by one time step (dt = 1); ma_half_time is
// in those steps. The chain's 1e18 fixed-point is the chain's concern; here the
// math runs in double with profit measured around 1.0.
class RepeggingCryptoswapPool : public CryptoswapPoolN
{
 public:
  RepeggingCryptoswapPool(std::vector<double> balances, std::vector<double> priceScale, double A,
                          double gamma, double maHalfTime, int32_t midFeeBps, int32_t outFeeBps,
                          double feeGamma, double allowedExtraProfit, double adjustmentStep)
      : CryptoswapPoolN(std::move(balances), std::move(priceScale), A, gamma, 0),
        _priceOracle(_scale),
        _lastPrice(_scale),
        _maHalfTime(maHalfTime),
        _midFee(static_cast<double>(midFeeBps) / 10000.0),
        _outFee(static_cast<double>(outFeeBps) / 10000.0),
        _feeGamma(feeGamma),
        _allowedExtraProfit(allowedExtraProfit),
        _adjustmentStep(adjustmentStep)
  {
    // Normalize the LP supply so the virtual price starts at 1.
    _totalSupply = xcpOf(getD(xpOf()), _scale);
    _virtualPrice = 1.0;
    _xcpProfit = 1.0;
  }

  const std::vector<double>& priceOracle() const { return _priceOracle; }
  double virtualPrice() const { return _virtualPrice; }
  double xcpProfit() const { return _xcpProfit; }

  // The fee fraction taken on a swap at the current balance, in basis points,
  // for inspection.
  double feeBpsNow() const { return (1.0 - feeFraction()) * 10000.0; }

  Quantity applySwap(std::size_t i, std::size_t j, Quantity amountIn) override
  {
    const Quantity out = CryptoswapPoolN::applySwap(i, j, amountIn);
    tweakPrice();
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<RepeggingCryptoswapPool>(*this);
  }

 protected:
  // Dynamic fee: K is 1 at balance and falls toward 0 as the pool goes
  // imbalanced; g = fee_gamma/(fee_gamma + 1 - K) weights mid_fee against
  // out_fee, so the taken fee runs from mid_fee (balanced) to out_fee (lopsided).
  double feeFraction() const override
  {
    const std::vector<double> xp = xpOf();
    const double n = static_cast<double>(xp.size());
    double sum = 0.0, prod = 1.0;
    for (double v : xp)
    {
      sum += v;
      prod *= v;
    }
    if (sum <= 0.0)
    {
      return 1.0 - _midFee;
    }
    const double mean = sum / n;
    double meanPow = 1.0;
    for (std::size_t k = 0; k < xp.size(); ++k)
    {
      meanPow *= mean;
    }
    const double K = prod / meanPow;  // 1 at balance, -> 0 imbalanced
    const double g = _feeGamma / (_feeGamma + 1.0 - K);
    const double fee = g * _midFee + (1.0 - g) * _outFee;
    return 1.0 - fee;
  }

 private:
  // X_cp = (Prod_i D/(N*price_scale_i))^(1/N), the constant-product virtual
  // value of the pool at a given scale; virtual_price = X_cp / total_supply.
  double xcpOf(double D, const std::vector<double>& scale) const
  {
    const double n = static_cast<double>(_b.size());
    double prod = 1.0;
    for (std::size_t k = 0; k < _b.size(); ++k)
    {
      const double sc = (k == 0) ? 1.0 : scale[k - 1];
      prod *= D / (n * sc);
    }
    return std::pow(prod, 1.0 / n);
  }

  // Transformed balances for an arbitrary scale (xpOf uses the live _scale).
  std::vector<double> xpForScale(const std::vector<double>& scale) const
  {
    std::vector<double> xp(_b.size());
    for (std::size_t k = 0; k < _b.size(); ++k)
    {
      xp[k] = _b[k] * ((k == 0) ? 1.0 : scale[k - 1]);
    }
    return xp;
  }

  // After a swap: advance the EMA oracle on the new marginal prices, update the
  // profit measures, and repeg the scale toward the oracle if the gate allows.
  void tweakPrice()
  {
    const std::size_t n = _b.size();
    for (std::size_t k = 1; k < n; ++k)
    {
      _lastPrice[k - 1] = spotPrice(0, k).toDouble();  // coin k priced in coin 0
    }
    const double alpha = std::pow(2.0, -1.0 / _maHalfTime);
    for (std::size_t k = 0; k + 1 < n; ++k)
    {
      _priceOracle[k] = _lastPrice[k] * (1.0 - alpha) + _priceOracle[k] * alpha;
    }

    const double D = getD(xpOf());
    const double vpNew = xcpOf(D, _scale) / _totalSupply;
    _xcpProfit *= vpNew / _virtualPrice;
    _virtualPrice = vpNew;

    // Gate (a): the pool must be far enough ahead to fund a rebalance.
    if (_virtualPrice * 2.0 - 1.0 <= _xcpProfit + 2.0 * _allowedExtraProfit)
    {
      return;
    }
    // Gate (b): the oracle must have drifted from the scale by more than a step.
    double norm = 0.0;
    for (std::size_t k = 0; k + 1 < n; ++k)
    {
      const double d = _priceOracle[k] / _scale[k] - 1.0;
      norm += d * d;
    }
    norm = std::sqrt(norm);
    if (norm <= _adjustmentStep)
    {
      return;
    }

    // Tentative step toward the oracle, accepted only if it leaves the pool in
    // profit (else the rebalance would burn LP value, so revert).
    std::vector<double> newScale(_scale.size());
    for (std::size_t k = 0; k < _scale.size(); ++k)
    {
      newScale[k] = (_scale[k] * (norm - _adjustmentStep) + _adjustmentStep * _priceOracle[k]) / norm;
    }
    const double D2 = getD(xpForScale(newScale));
    const double vp2 = xcpOf(D2, newScale) / _totalSupply;
    if (vp2 > 1.0 && 2.0 * vp2 - 1.0 > _xcpProfit)
    {
      _scale = newScale;
      _virtualPrice = vp2;
    }
  }

  std::vector<double> _priceOracle;
  std::vector<double> _lastPrice;
  double _maHalfTime;
  double _midFee;
  double _outFee;
  double _feeGamma;
  double _allowedExtraProfit;
  double _adjustmentStep;
  double _totalSupply = 1.0;
  double _virtualPrice = 1.0;
  double _xcpProfit = 1.0;
};

}  // namespace flox
