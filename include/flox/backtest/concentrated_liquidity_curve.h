/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/amm_curve.h"
#include "flox/common.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace flox
{

// Concentrated-liquidity curve (Uniswap v3 / Orca / Raydium style), the
// dominant 2026 DEX model. Liquidity sits in price ranges, so the active
// liquidity L is constant only between two adjacent initialized ticks. A swap
// walks tick by tick: within a range it is constant-product on the virtual
// reserves L/sqrt(P) and L*sqrt(P); crossing an initialized tick changes L by
// the tick's liquidityNet. base is token0, quote is token1, price = quote/base.
//
// State is the current sqrt(price), the active liquidity, and the tick table.
// The math runs in double, which is enough for a backtest; the on-chain Q64.96
// fixed-point is the chain's concern, not the model's.
struct ClTick
{
  double price;         // tick price (quote per base)
  double liquidityNet;  // change in active L when crossing this tick upward
};

class ConcentratedLiquidityCurve : public IAmmCurve
{
 public:
  ConcentratedLiquidityCurve(double price, double liquidity, std::vector<ClTick> ticks,
                             int32_t feeBps)
      : _sqrtP(std::sqrt(price)), _liquidity(liquidity), _feeBps(feeBps)
  {
    _ticks.reserve(ticks.size());
    for (const auto& t : ticks)
    {
      _ticks.push_back({std::sqrt(t.price), t.liquidityNet});
    }
    std::sort(_ticks.begin(), _ticks.end(),
              [](const Tick& a, const Tick& b)
              { return a.sqrtP < b.sqrtP; });
  }

  double price() const { return _sqrtP * _sqrtP; }
  double liquidity() const { return _liquidity; }

  Price spotPrice() const override { return Price::fromDouble(_sqrtP * _sqrtP); }

  Quantity amountOut(Quantity amountIn, bool baseForQuote) const override
  {
    double sp = _sqrtP;
    double L = _liquidity;
    return Quantity::fromDouble(walk(amountIn.toDouble(), baseForQuote, sp, L));
  }

  double priceImpact(Quantity amountIn, bool baseForQuote) const override
  {
    const double in = amountIn.toDouble();
    const double out = amountOut(amountIn, baseForQuote).toDouble();
    if (in <= 0.0 || out <= 0.0)
    {
      return 0.0;
    }
    const double p = _sqrtP * _sqrtP;
    const double spotRate = baseForQuote ? p : 1.0 / p;  // marginal out per in, no fee
    const double execRate = out / in;
    return 1.0 - execRate / spotRate;
  }

  Quantity applySwap(Quantity amountIn, bool baseForQuote) override
  {
    return Quantity::fromDouble(walk(amountIn.toDouble(), baseForQuote, _sqrtP, _liquidity));
  }

 private:
  struct Tick
  {
    double sqrtP;
    double liquidityNet;
  };

  // Walk the swap across tick boundaries, mutating sp and L (passed by
  // reference). Returns the total output. Fee is taken off the input up front.
  double walk(double amountIn, bool baseForQuote, double& sp, double& L) const
  {
    if (amountIn <= 0.0 || L <= 0.0)
    {
      return 0.0;
    }
    double rem = amountIn * (1.0 - static_cast<double>(_feeBps) / 10000.0);
    double out = 0.0;

    if (baseForQuote)
    {
      // token0 in, price falls. Walk ticks downward.
      auto lb = std::lower_bound(_ticks.begin(), _ticks.end(), sp,
                                 [](const Tick& t, double v)
                                 { return t.sqrtP < v; });
      int i = static_cast<int>(lb - _ticks.begin()) - 1;  // highest tick strictly below sp
      while (rem > 0.0 && L > 0.0)
      {
        const double boundary = (i >= 0) ? _ticks[i].sqrtP : 0.0;
        const double dxToBoundary =
            (boundary > 0.0) ? L * (sp - boundary) / (sp * boundary)
                             : std::numeric_limits<double>::infinity();
        if (boundary > 0.0 && rem >= dxToBoundary)
        {
          out += L * (sp - boundary);
          rem -= dxToBoundary;
          sp = boundary;
          L -= _ticks[i].liquidityNet;  // cross down
          --i;
        }
        else
        {
          const double spNew = L * sp / (L + rem * sp);
          out += L * (sp - spNew);
          sp = spNew;
          rem = 0.0;
        }
      }
    }
    else
    {
      // token1 in, price rises. Walk ticks upward.
      auto ub = std::upper_bound(_ticks.begin(), _ticks.end(), sp,
                                 [](double v, const Tick& t)
                                 { return v < t.sqrtP; });
      int i = static_cast<int>(ub - _ticks.begin());  // first tick strictly above sp
      const int n = static_cast<int>(_ticks.size());
      while (rem > 0.0 && L > 0.0)
      {
        const double boundary =
            (i < n) ? _ticks[i].sqrtP : std::numeric_limits<double>::infinity();
        const double dyToBoundary =
            std::isinf(boundary) ? std::numeric_limits<double>::infinity() : L * (boundary - sp);
        if (!std::isinf(boundary) && rem >= dyToBoundary)
        {
          out += L * (boundary - sp) / (sp * boundary);
          rem -= dyToBoundary;
          sp = boundary;
          L += _ticks[i].liquidityNet;  // cross up
          ++i;
        }
        else
        {
          const double spNew = sp + rem / L;
          out += L * (spNew - sp) / (sp * spNew);
          sp = spNew;
          rem = 0.0;
        }
      }
    }
    return out;
  }

  double _sqrtP;
  double _liquidity;
  std::vector<Tick> _ticks;
  int32_t _feeBps;
};

}  // namespace flox
