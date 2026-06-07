/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/constant_product_curve.h"
#include "flox/connector/amm_dex_connector.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

// 18-decimal tokens, reserves 1000 base / 2000 quote, so the marginal price is
// about 2 quote per base. levelSize 1 base.
ConstantProductCurve makePool()
{
  return ConstantProductCurve(u256::fromDec("1000000000000000000000"),
                              u256::fromDec("2000000000000000000000"), 997, 1000);
}

TEST(AmmDexConnectorTest, CurveStateSynthesizesBook)
{
  // No fee, so the marginal price is exactly the reserve ratio (~2) and the level
  // spread comes purely from depth impact.
  ConstantProductCurve curve(u256::fromDec("1000000000000000000000"),
                             u256::fromDec("2000000000000000000000"), 1, 1);
  AmmDexConnector conn("amm", 1, curve, 0, 1, 18, 18, 3, u256::fromDec("1000000000000000000"));

  bool gotBook = false;
  std::vector<BookLevel> bids, asks;
  conn.setCallbacks(
      [&](const BookUpdateEvent& ev)
      {
        gotBook = true;
        bids.assign(ev.update.bids.begin(), ev.update.bids.end());
        asks.assign(ev.update.asks.begin(), ev.update.asks.end());
      },
      [](const TradeEvent&) {});

  conn.republish();

  ASSERT_TRUE(gotBook);
  ASSERT_EQ(bids.size(), 3u);
  ASSERT_EQ(asks.size(), 3u);
  const double spot = 2.0;
  EXPECT_NEAR(asks[0].price.toDouble(), spot, 0.05);
  EXPECT_LT(bids[0].price.toDouble(), spot);
  EXPECT_GT(asks[0].price.toDouble(), spot);
  EXPECT_LT(bids[1].price.toDouble(), bids[0].price.toDouble());
  EXPECT_GT(asks[1].price.toDouble(), asks[0].price.toDouble());
}

TEST(AmmDexConnectorTest, SwapEmitsTrade)
{
  // No fee (feeNum == feeDen), reserves 1000/1000.
  ConstantProductCurve curve(u256::fromDec("1000000000000000000000"),
                             u256::fromDec("1000000000000000000000"), 1, 1);
  AmmDexConnector conn("amm", 1, curve, 0, 1, 18, 18, 2, u256::fromDec("1000000000000000000"));

  bool gotTrade = false;
  TradeEvent captured;
  conn.setCallbacks([](const BookUpdateEvent&) {},
                    [&](const TradeEvent& ev)
                    {
                      gotTrade = true;
                      captured = ev;
                    });

  conn.onSwap(u256::fromDec("10000000000000000000"), true, 12345);  // sell 10 base

  ASSERT_TRUE(gotTrade);
  EXPECT_EQ(captured.trade.symbol, 1u);
  EXPECT_FALSE(captured.trade.isBuy);  // sold base into the pool
  EXPECT_NEAR(captured.trade.quantity.toDouble(), 10.0, 1e-6);
  EXPECT_GT(captured.trade.price.toDouble(), 0.0);
  EXPECT_EQ(captured.trade.exchangeTsNs, 12345u);
}

TEST(AmmDexConnectorTest, ExchangeId)
{
  ConstantProductCurve curve = makePool();
  AmmDexConnector conn("uniswap", 1, curve, 0, 1, 18, 18, 1, u256::fromDec("1000000000000000000"));
  EXPECT_EQ(conn.exchangeId(), "uniswap");
}

// The connector prices through the INTokenCurve interface, so any curve drives it.
TEST(AmmDexConnectorTest, PricesOverInterfaceReference)
{
  ConstantProductCurve cp = makePool();
  INTokenCurve& curve = cp;
  AmmDexConnector conn("amm", 1, curve, 0, 1, 18, 18, 1, u256::fromDec("1000000000000000000"));

  bool gotBook = false;
  conn.setCallbacks([&](const BookUpdateEvent&)
                    { gotBook = true; }, [](const TradeEvent&) {});
  conn.republish();
  EXPECT_TRUE(gotBook);
}

}  // namespace
