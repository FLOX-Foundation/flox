/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/amm_curve.h"
#include "flox/backtest/amm_pricing.h"
#include "flox/backtest/concentrated_liquidity_curve.h"
#include "flox/backtest/cryptoswap_curve.h"
#include "flox/backtest/stableswap_curve.h"
#include "flox/backtest/weighted_curve.h"

#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace flox;

namespace
{

std::vector<std::unique_ptr<IAmmCurve>> allCurves()
{
  const auto q = [](double d)
  { return Quantity::fromDouble(d); };
  std::vector<std::unique_ptr<IAmmCurve>> v;
  v.push_back(std::make_unique<ConstantProductCurve>(q(1e6), q(1e6), 0));
  // base 4M / quote 1M with 0.8/0.2 weights also sits at spot 1: (1e6/0.2)/(4e6/0.8) = 1.
  v.push_back(std::make_unique<WeightedCurve>(q(4e6), q(1e6), 0.8, 0.2, 0));
  v.push_back(std::make_unique<StableSwapCurve>(q(1e6), q(1e6), 100.0, 0));
  v.push_back(std::make_unique<CryptoswapCurve>(q(1e6), q(1e6), 100.0, 0.1, 0));
  v.push_back(std::make_unique<ConcentratedLiquidityCurve>(
      1.0, 1e6, std::vector<ClTick>{{0.25, 1e6}, {4.0, -1e6}}, 0));
  return v;
}

// Mutating the clone must not touch the original, and vice versa.
TEST(AmmCurveCloneTest, CloneIsIndependent)
{
  for (auto& c : allCurves())
  {
    const Price spotBefore = c->spotPrice();
    auto copy = c->clone();
    EXPECT_NEAR(copy->spotPrice().toDouble(), spotBefore.toDouble(), 1e-9);

    copy->applySwap(Quantity::fromDouble(100'000.0), true);               // move the clone
    EXPECT_NEAR(c->spotPrice().toDouble(), spotBefore.toDouble(), 1e-9);  // original intact
    EXPECT_LT(copy->spotPrice().toDouble(), spotBefore.toDouble());       // clone moved

    // And the other direction: mutating the original leaves an earlier clone put.
    auto pinned = c->clone();
    c->applySwap(Quantity::fromDouble(100'000.0), true);
    EXPECT_NEAR(pinned->spotPrice().toDouble(), spotBefore.toDouble(), 1e-9);
  }
}

// The reason clone exists: size a swap to a target spot price using only the
// IAmmCurve interface, by bisecting on throwaway clones. Quote-in raises the
// quote-per-base spot, and it is monotone in size.
double quoteInToReachSpot(const IAmmCurve& start, double target)
{
  double lo = 0.0, hi = 1e6;
  for (int i = 0; i < 80; ++i)
  {
    const double mid = 0.5 * (lo + hi);
    auto probe = start.clone();
    probe->applySwap(Quantity::fromDouble(mid), false);
    if (probe->spotPrice().toDouble() < target)
    {
      lo = mid;
    }
    else
    {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

TEST(AmmCurveCloneTest, GenericArbToTargetSpot)
{
  const double target = 1.05;
  for (auto& c : allCurves())
  {
    const double in = quoteInToReachSpot(*c, target);
    EXPECT_NEAR(c->spotPrice().toDouble(), 1.0, 1e-6);  // search left the curve untouched
    c->applySwap(Quantity::fromDouble(in), false);
    EXPECT_NEAR(c->spotPrice().toDouble(), target, 1e-3);  // and the size hits the target
  }
}

}  // namespace
