/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/cryptoswap_pool_n.h"
#include "flox/backtest/repegging_cryptoswap_pool.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

// Helpers to build a pool with the repegging off (huge adjustment step) or on.
RepeggingCryptoswapPool pinned(int32_t midFee, int32_t outFee)
{
  return RepeggingCryptoswapPool({1'000'000.0, 1'000'000.0}, {1.0}, 100.0, 0.1,
                                 /*maHalfTime*/ 10.0, midFee, outFee, /*feeGamma*/ 0.001,
                                 /*allowedExtraProfit*/ 1e-9, /*adjustmentStep*/ 1e9);
}

// With zero fee and the scale pinned, the repegging pool is the static pool.
TEST(RepeggingCryptoswapPoolTest, ReducesToStaticWhenPinned)
{
  RepeggingCryptoswapPool rp = pinned(0, 0);
  CryptoswapPoolN st({1'000'000.0, 1'000'000.0}, {1.0}, 100.0, 0.1, 0);
  EXPECT_NEAR(rp.amountOut(0, 1, Quantity::fromDouble(200'000.0)).toDouble(),
              st.amountOut(0, 1, Quantity::fromDouble(200'000.0)).toDouble(), 1e-6);
}

// The fee is mid_fee at balance and climbs toward out_fee as the pool tips.
TEST(RepeggingCryptoswapPoolTest, DynamicFeeRisesWithImbalance)
{
  RepeggingCryptoswapPool rp = pinned(4, 40);
  const double balanced = rp.feeBpsNow();
  EXPECT_NEAR(balanced, 4.0, 0.5);
  rp.applySwap(0, 1, Quantity::fromDouble(500'000.0));  // tip it hard
  const double tipped = rp.feeBpsNow();
  EXPECT_GT(tipped, balanced);
  EXPECT_LT(tipped, 40.0 + 1e-6);
}

// No fee means no profit, so the gate never opens and the scale cannot move,
// however far the price is pushed.
TEST(RepeggingCryptoswapPoolTest, NoRepegWithoutProfit)
{
  RepeggingCryptoswapPool rp({1'000'000.0, 1'000'000.0}, {1.0}, 100.0, 0.1, 10.0, 0, 0, 0.001, 1e-9,
                             1e-4);
  for (int t = 0; t < 100; ++t)
  {
    rp.applySwap(0, 1, Quantity::fromDouble(3'000.0));
  }
  EXPECT_NEAR(rp.priceScale()[0], 1.0, 1e-12);  // pinned by the profit gate
  EXPECT_GT(rp.priceOracle()[0], 1.0);          // the oracle still tracked the price
}

// With fees on, a run of one-sided buys raises the traded price; the oracle
// follows and the scale repegs upward toward it, funded by the fees.
TEST(RepeggingCryptoswapPoolTest, RepegDriftsScaleTowardPrice)
{
  RepeggingCryptoswapPool rp({1'000'000.0, 1'000'000.0}, {1.0}, 100.0, 0.1, 10.0, 4, 40, 0.001, 1e-9,
                             1e-4);
  for (int t = 0; t < 300; ++t)
  {
    rp.applySwap(0, 1, Quantity::fromDouble(3'000.0));
  }
  EXPECT_GT(rp.priceScale()[0], 1.0);                  // scale drifted up
  EXPECT_GT(rp.priceOracle()[0], rp.priceScale()[0]);  // toward the (higher) oracle
  EXPECT_GT(rp.xcpProfit(), 1.0);                      // funded by accumulated fees
}

// A repeg never spends the LPs' fee income: xcp_profit only grows.
TEST(RepeggingCryptoswapPoolTest, XcpProfitNeverDecreases)
{
  RepeggingCryptoswapPool rp({1'000'000.0, 1'000'000.0}, {1.0}, 100.0, 0.1, 10.0, 4, 40, 0.001, 1e-9,
                             1e-4);
  double prev = rp.xcpProfit();
  for (int t = 0; t < 200; ++t)
  {
    rp.applySwap(t % 2 == 0 ? 0 : 1, t % 2 == 0 ? 1 : 0, Quantity::fromDouble(20'000.0));
    EXPECT_GE(rp.xcpProfit(), prev - 1e-12);
    prev = rp.xcpProfit();
  }
  EXPECT_GT(rp.xcpProfit(), 1.0);
}

TEST(RepeggingCryptoswapPoolTest, CloneIsIndependent)
{
  RepeggingCryptoswapPool rp({1'000'000.0, 1'000'000.0}, {1.0}, 100.0, 0.1, 10.0, 4, 40, 0.001, 1e-9,
                             1e-4);
  for (int t = 0; t < 50; ++t)
  {
    rp.applySwap(0, 1, Quantity::fromDouble(5'000.0));
  }
  const double scaleBefore = rp.priceScale()[0];
  auto copy = rp.clone();
  for (int t = 0; t < 50; ++t)
  {
    copy->applySwap(0, 1, Quantity::fromDouble(5'000.0));
  }
  EXPECT_NEAR(rp.priceScale()[0], scaleBefore, 1e-12);  // original untouched by the clone
}

}  // namespace
