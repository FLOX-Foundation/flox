/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/strategy/quoter.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

Quoter q(int32_t halfSpreadBps, int levels, int32_t stepBps, double skew)
{
  return Quoter(halfSpreadBps, levels, Quantity::fromDouble(1.0), stepBps, skew);
}

TEST(QuoterTest, SymmetricNoSkew)
{
  auto quoter = q(10, 1, 5, 0.0);  // 10 bps half-spread
  auto levels = quoter.quotes(Price::fromDouble(100.0), Quantity{});
  ASSERT_EQ(levels.size(), 2u);
  // levels[0] = bid, levels[1] = ask.
  EXPECT_EQ(levels[0].side, Side::BUY);
  EXPECT_EQ(levels[1].side, Side::SELL);
  EXPECT_NEAR(levels[0].price.toDouble(), 99.9, 1e-6);
  EXPECT_NEAR(levels[1].price.toDouble(), 100.1, 1e-6);
  // Midpoint is the fair price.
  EXPECT_NEAR((levels[0].price.toDouble() + levels[1].price.toDouble()) / 2.0, 100.0, 1e-6);
}

TEST(QuoterTest, LevelsStepOut)
{
  auto quoter = q(10, 2, 5, 0.0);  // 2 levels, +5 bps per level
  auto levels = quoter.quotes(Price::fromDouble(100.0), Quantity{});
  ASSERT_EQ(levels.size(), 4u);
  // levels: bid0, ask0, bid1, ask1.
  EXPECT_LT(levels[2].price.toDouble(), levels[0].price.toDouble());  // bid1 < bid0
  EXPECT_GT(levels[3].price.toDouble(), levels[1].price.toDouble());  // ask1 > ask0
  EXPECT_NEAR(levels[2].price.toDouble(), 99.85, 1e-6);               // 100 * (1 - 0.0015)
}

TEST(QuoterTest, InventorySkewLongPullsQuotesDown)
{
  auto quoter = q(10, 1, 5, 1.0);  // 1 bps per unit of inventory
  auto flat = quoter.quotes(Price::fromDouble(100.0), Quantity{});
  auto longInv = quoter.quotes(Price::fromDouble(100.0), Quantity::fromDouble(10.0));
  // 10 units long -> reservation = 100 * (1 - 10bps) = 99.9; both quotes lower.
  EXPECT_LT(longInv[0].price.toDouble(), flat[0].price.toDouble());
  EXPECT_LT(longInv[1].price.toDouble(), flat[1].price.toDouble());
}

TEST(QuoterTest, ReservationPriceSign)
{
  auto quoter = q(10, 1, 5, 1.0);
  Price fair = Price::fromDouble(100.0);
  EXPECT_NEAR(quoter.reservationPrice(fair, Quantity{}).toDouble(), 100.0, 1e-6);
  EXPECT_LT(quoter.reservationPrice(fair, Quantity::fromDouble(5.0)).toDouble(), 100.0);
  EXPECT_GT(quoter.reservationPrice(fair, Quantity::fromDouble(-5.0)).toDouble(), 100.0);
}

// Regression: a large inventory must not drive the reservation price or the
// ladder negative. The skew is capped.
TEST(QuoterTest, LargeInventoryKeepsQuotesPositive)
{
  auto quoter = q(20, 3, 10, 0.5);  // 0.5 bps per unit of inventory
  Price fair = Price::fromDouble(100.0);
  // A billion units of inventory: uncapped, this would push the price far
  // negative.
  Quantity huge = Quantity::fromDouble(1'000'000'000.0);
  Price res = quoter.reservationPrice(fair, huge);
  EXPECT_GT(res.toDouble(), 0.0);

  auto levels = quoter.quotes(fair, huge);
  ASSERT_FALSE(levels.empty());
  for (const auto& lvl : levels)
  {
    EXPECT_GT(lvl.price.toDouble(), 0.0);
  }
}

TEST(QuoterTest, ShouldRequoteRespectsTolerance)
{
  Price a = Price::fromDouble(100.0);
  Price near = Price::fromRaw(a.raw() + 3);   // 3 ticks away
  Price far = Price::fromRaw(a.raw() + 100);  // 100 ticks away
  EXPECT_FALSE(Quoter::shouldRequote(a, near, 5));
  EXPECT_TRUE(Quoter::shouldRequote(a, far, 5));
}

}  // namespace
