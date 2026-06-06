/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/strategy/signal.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

TEST(LpSignalTest, ProvideLiquidityCarriesRangeAndAmount)
{
  Price lo = Price::fromDouble(1900.0);
  Price hi = Price::fromDouble(2100.0);
  Quantity liq = Quantity::fromDouble(5.0);
  Signal s = Signal::provideLiquidity(42, lo, hi, liq, 7);

  EXPECT_EQ(s.type, SignalType::ProvideLiquidity);
  EXPECT_EQ(s.symbol, 42u);
  EXPECT_EQ(s.priceLower.raw(), lo.raw());
  EXPECT_EQ(s.priceUpper.raw(), hi.raw());
  EXPECT_EQ(s.liquidity.raw(), liq.raw());
  EXPECT_EQ(s.orderId, 7u);
}

TEST(LpSignalTest, WithdrawLiquidityCarriesAmount)
{
  Quantity liq = Quantity::fromDouble(2.5);
  Signal s = Signal::withdrawLiquidity(42, liq, 8);

  EXPECT_EQ(s.type, SignalType::WithdrawLiquidity);
  EXPECT_EQ(s.symbol, 42u);
  EXPECT_EQ(s.liquidity.raw(), liq.raw());
  EXPECT_EQ(s.orderId, 8u);
}

// LP signals leave the CEX order fields untouched, so a consumer that
// only reads price/quantity does not act on them as if they were orders.
TEST(LpSignalTest, LpSignalLeavesOrderFieldsZero)
{
  Signal s = Signal::provideLiquidity(1, Price::fromDouble(1.0), Price::fromDouble(2.0),
                                      Quantity::fromDouble(1.0), 1);
  EXPECT_EQ(s.price.raw(), 0);
  EXPECT_EQ(s.quantity.raw(), 0);
  EXPECT_EQ(s.triggerPrice.raw(), 0);
}

}  // namespace
