/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/ntoken_curve.h"

#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace flox;

namespace
{

// A minimal stub that prices each token pair as an independent constant-product
// curve on the two reserves. Not a real model -- it only exists to exercise the
// INTokenCurve interface shape (indices, clone, the four queries) before the
// real pools (W19-T002+) implement it.
class NTokenConstProductStub : public INTokenCurve
{
 public:
  explicit NTokenConstProductStub(std::vector<double> reserves) : _r(std::move(reserves)) {}

  std::size_t tokenCount() const override { return _r.size(); }

  Price spotPrice(std::size_t i, std::size_t j) const override
  {
    return Price::fromDouble(_r[i] / _r[j]);  // units of i per unit of j
  }

  Quantity amountOut(std::size_t i, std::size_t j, Quantity amountIn) const override
  {
    const double in = amountIn.toDouble();
    return Quantity::fromDouble(_r[j] * in / (_r[i] + in));
  }

  double priceImpact(std::size_t i, std::size_t j, Quantity amountIn) const override
  {
    const double in = amountIn.toDouble();
    const double out = amountOut(i, j, amountIn).toDouble();
    if (in <= 0.0 || out <= 0.0)
    {
      return 0.0;
    }
    const double spotRate = _r[j] / _r[i];  // out per in, marginal
    return 1.0 - (out / in) / spotRate;
  }

  Quantity applySwap(std::size_t i, std::size_t j, Quantity amountIn) override
  {
    const Quantity out = amountOut(i, j, amountIn);
    _r[i] += amountIn.toDouble();
    _r[j] -= out.toDouble();
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<NTokenConstProductStub>(*this);
  }

 private:
  std::vector<double> _r;
};

TEST(NTokenCurveTest, InterfaceShape)
{
  NTokenConstProductStub pool(std::vector<double>{1000.0, 2000.0, 4000.0});
  EXPECT_EQ(pool.tokenCount(), 3u);

  // Reciprocal pricing: price of j in i times price of i in j is 1.
  EXPECT_NEAR(pool.spotPrice(0, 2).toDouble() * pool.spotPrice(2, 0).toDouble(), 1.0, 1e-9);

  const Quantity out = pool.amountOut(0, 1, Quantity::fromDouble(10.0));
  EXPECT_GT(out.toDouble(), 0.0);
  EXPECT_LT(out.toDouble(), 2000.0);

  EXPECT_GT(pool.priceImpact(0, 1, Quantity::fromDouble(500.0)),
            pool.priceImpact(0, 1, Quantity::fromDouble(5.0)));
}

TEST(NTokenCurveTest, CloneIsIndependent)
{
  NTokenConstProductStub pool(std::vector<double>{1000.0, 1000.0, 1000.0});
  const double spotBefore = pool.spotPrice(0, 1).toDouble();

  auto copy = pool.clone();
  copy->applySwap(0, 1, Quantity::fromDouble(200.0));              // move the clone
  EXPECT_NEAR(pool.spotPrice(0, 1).toDouble(), spotBefore, 1e-9);  // original intact
  EXPECT_NE(copy->spotPrice(0, 1).toDouble(), spotBefore);         // clone moved
}

}  // namespace
