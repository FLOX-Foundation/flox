/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/amm_pricing.h"
#include "flox/connector/amm_dex_connector.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

TEST(AmmDexConnectorTest, CurveStateSynthesizesBook)
{
  ConstantProductCurve curve(Quantity::fromDouble(1000.0), Quantity::fromDouble(2000.0), 30);
  AmmDexConnector conn("amm", 1, curve, 3, Quantity::fromDouble(1.0));

  bool gotBook = false;
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
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
  const double spot = 2.0;  // quote / base
  EXPECT_LT(bids[0].price.toDouble(), spot);
  EXPECT_GT(asks[0].price.toDouble(), spot);
  EXPECT_LT(bids[1].price.toDouble(), bids[0].price.toDouble());
  EXPECT_GT(asks[1].price.toDouble(), asks[0].price.toDouble());
}

TEST(AmmDexConnectorTest, SwapEmitsTrade)
{
  ConstantProductCurve curve(Quantity::fromDouble(1000.0), Quantity::fromDouble(1000.0), 0);
  AmmDexConnector conn("amm", 1, curve, 2, Quantity::fromDouble(1.0));

  bool gotTrade = false;
  TradeEvent captured;
  conn.setCallbacks([](const BookUpdateEvent&) {},
                    [&](const TradeEvent& ev)
                    {
                      gotTrade = true;
                      captured = ev;
                    });

  conn.onSwap(Quantity::fromDouble(10.0), true, 12345);  // sell 10 base

  ASSERT_TRUE(gotTrade);
  EXPECT_EQ(captured.trade.symbol, 1u);
  EXPECT_FALSE(captured.trade.isBuy);  // sold base into the pool
  EXPECT_NEAR(captured.trade.quantity.toDouble(), 10.0, 1e-6);
  EXPECT_GT(captured.trade.price.toDouble(), 0.0);
  EXPECT_EQ(captured.trade.exchangeTsNs, 12345u);
}

TEST(AmmDexConnectorTest, ExchangeId)
{
  ConstantProductCurve curve(Quantity::fromDouble(1.0), Quantity::fromDouble(1.0), 0);
  AmmDexConnector conn("uniswap", 1, curve, 1, Quantity::fromDouble(1.0));
  EXPECT_EQ(conn.exchangeId(), "uniswap");
}

// The connector prices through the IAmmCurve interface, so any curve drives it.
TEST(AmmDexConnectorTest, PricesOverInterfaceReference)
{
  ConstantProductCurve cp(Quantity::fromDouble(1000.0), Quantity::fromDouble(1000.0), 0);
  IAmmCurve& curve = cp;
  AmmDexConnector conn("amm", 1, curve, 1, Quantity::fromDouble(1.0));

  bool gotBook = false;
  conn.setCallbacks([&](const BookUpdateEvent&)
                    { gotBook = true; }, [](const TradeEvent&) {});
  conn.republish();
  EXPECT_TRUE(gotBook);
}

}  // namespace
