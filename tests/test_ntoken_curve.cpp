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

// A minimal stub that prices each pair as an independent constant-product curve
// on the two balances, no fee. Exists only to exercise the INTokenCurve interface
// shape (indices, u256 amounts, balances, clone).
class NTokenStub : public INTokenCurve
{
 public:
  explicit NTokenStub(std::vector<u256> balances) : _b(std::move(balances)) {}

  std::size_t tokenCount() const override { return _b.size(); }
  const std::vector<u256>& balances() const override { return _b; }

  u256 amountOut(std::size_t i, std::size_t j, const u256& amountIn) const override
  {
    if (amountIn.isZero())
    {
      return u256(0);
    }
    return mulDiv(_b[j], amountIn, _b[i] + amountIn);  // floor
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
    return std::make_unique<NTokenStub>(*this);
  }

 private:
  std::vector<u256> _b;
};

// balances() on the interface lets generic code value a pool at given per-token
// prices without the concrete type.
u256 poolValueScaled(const INTokenCurve& pool, const std::vector<u256>& priceScaled)
{
  const auto& b = pool.balances();
  u256 v(0);
  for (std::size_t k = 0; k < b.size(); ++k)
  {
    v = v + b[k] * priceScaled[k];
  }
  return v;
}

TEST(NTokenCurveTest, InterfaceShape)
{
  NTokenStub pool(std::vector<u256>{u256(1000), u256(2000), u256(4000)});
  EXPECT_EQ(pool.tokenCount(), 3u);

  const u256 out = pool.amountOut(0, 1, u256(10));
  EXPECT_FALSE(out.isZero());
  EXPECT_TRUE(out < u256(2000));

  // Valuation through the interface: balances [1000,2000,4000] at prices [2,1,1]
  // = 2000 + 2000 + 4000 = 8000.
  EXPECT_EQ(poolValueScaled(pool, {u256(2), u256(1), u256(1)}).toDec(), "8000");
}

TEST(NTokenCurveTest, CloneIsIndependent)
{
  NTokenStub pool(std::vector<u256>{u256(1000), u256(1000), u256(1000)});
  auto copy = pool.clone();
  copy->applySwap(0, 1, u256(200));
  EXPECT_EQ(pool.balances()[0].toDec(), "1000");   // original intact
  EXPECT_EQ(copy->balances()[0].toDec(), "1200");  // clone moved
}

}  // namespace
