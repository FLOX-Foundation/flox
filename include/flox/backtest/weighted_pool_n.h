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
#include "flox/common.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace flox
{

// Balancer-style weighted pool with n assets, as an INTokenCurve. The invariant
// is Prod(B_k^w_k) = const with the weights summing to 1. The swap formula
// depends only on the in-token and the out-token, so an n-asset pool prices each
// pair with the same closed form as the two-token WeightedCurve -- the extra
// assets only sit in the state. A swap between i and j leaves every other
// balance untouched.
class WeightedPoolN : public INTokenCurve
{
 public:
  WeightedPoolN(std::vector<double> balances, std::vector<double> weights, int32_t feeBps)
      : _b(std::move(balances)), _w(std::move(weights)), _feeBps(feeBps)
  {
  }

  const std::vector<double>& balances() const { return _b; }
  const std::vector<double>& weights() const { return _w; }
  int32_t feeBps() const { return _feeBps; }

  std::size_t tokenCount() const override { return _b.size(); }

  // Marginal token-i per token-j: (B_i/w_i) / (B_j/w_j).
  Price spotPrice(std::size_t i, std::size_t j) const override
  {
    if (_b[j] <= 0.0 || _w[i] <= 0.0 || _w[j] <= 0.0)
    {
      return Price{};
    }
    return Price::fromDouble((_b[i] / _w[i]) / (_b[j] / _w[j]));
  }

  Quantity amountOut(std::size_t i, std::size_t j, Quantity amountIn) const override
  {
    const double in = amountIn.toDouble();
    if (in <= 0.0 || _b[i] <= 0.0 || _b[j] <= 0.0 || _w[i] <= 0.0 || _w[j] <= 0.0)
    {
      return Quantity{};
    }
    const double inWithFee = in * (1.0 - static_cast<double>(_feeBps) / 10000.0);
    const double out = _b[j] * (1.0 - std::pow(_b[i] / (_b[i] + inWithFee), _w[i] / _w[j]));
    return Quantity::fromDouble(out);
  }

  double priceImpact(std::size_t i, std::size_t j, Quantity amountIn) const override
  {
    const double in = amountIn.toDouble();
    const double out = amountOut(i, j, amountIn).toDouble();
    if (in <= 0.0 || out <= 0.0 || _w[i] <= 0.0 || _w[j] <= 0.0)
    {
      return 0.0;
    }
    const double spotRate = (_b[j] / _w[j]) / (_b[i] / _w[i]);  // out per in, no fee
    return 1.0 - (out / in) / spotRate;
  }

  Quantity applySwap(std::size_t i, std::size_t j, Quantity amountIn) override
  {
    const Quantity out = amountOut(i, j, amountIn);
    _b[i] += amountIn.toDouble();
    _b[j] -= out.toDouble();
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<WeightedPoolN>(*this);
  }

 private:
  std::vector<double> _b;
  std::vector<double> _w;
  int32_t _feeBps;
};

}  // namespace flox
