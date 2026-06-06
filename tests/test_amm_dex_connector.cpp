/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/connector/amm_dex_connector.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

AmmDexConnector makeConn(const char* name, double base, double quote, int feeBps, int levels)
{
  return AmmDexConnector(name, 1, AmmPool(Quantity::fromDouble(base), Quantity::fromDouble(quote), feeBps),
                         levels, Quantity::fromDouble(1.0));
}

TEST(AmmDexConnectorTest, PoolStateSynthesizesBook)
{
  auto conn = makeConn("amm", 1000.0, 2000.0, 30, 3);

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

  conn.onPoolState(Quantity::fromDouble(1000.0), Quantity::fromDouble(2000.0));

  ASSERT_TRUE(gotBook);
  ASSERT_EQ(bids.size(), 3u);
  ASSERT_EQ(asks.size(), 3u);
  const double spot = 2.0;  // quote / base
  EXPECT_LT(bids[0].price.toDouble(), spot);
  EXPECT_GT(asks[0].price.toDouble(), spot);
  // Levels step away from spot.
  EXPECT_LT(bids[1].price.toDouble(), bids[0].price.toDouble());
  EXPECT_GT(asks[1].price.toDouble(), asks[0].price.toDouble());
}

TEST(AmmDexConnectorTest, SwapEmitsTrade)
{
  auto conn = makeConn("amm", 1000.0, 1000.0, 0, 2);

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
  auto conn = makeConn("uniswap", 1.0, 1.0, 0, 1);
  EXPECT_EQ(conn.exchangeId(), "uniswap");
}

}  // namespace
