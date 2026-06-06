/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/engine/symbol_registry.h"
#include "flox/exchange/venue_behavior.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

TEST(VenueBehaviorTest, IsDex)
{
  EXPECT_FALSE(isDex(VenueType::CentralizedExchange));
  EXPECT_TRUE(isDex(VenueType::AmmDex));
  EXPECT_TRUE(isDex(VenueType::HybridDex));
}

TEST(VenueBehaviorTest, BehaviorTable)
{
  auto cex = venueBehavior(VenueType::CentralizedExchange);
  EXPECT_TRUE(cex.usesOrderBook);
  EXPECT_FALSE(cex.onChainSettlement);

  auto amm = venueBehavior(VenueType::AmmDex);
  EXPECT_FALSE(amm.usesOrderBook);
  EXPECT_TRUE(amm.onChainSettlement);

  auto hybrid = venueBehavior(VenueType::HybridDex);
  EXPECT_TRUE(hybrid.usesOrderBook);
  EXPECT_TRUE(hybrid.onChainSettlement);
}

TEST(VenueBehaviorTest, VenueTypeForSymbol)
{
  SymbolRegistry reg;
  ExchangeId dexId = reg.registerExchange("uniswap", VenueType::AmmDex);
  ExchangeId cexId = reg.registerExchange("binance", VenueType::CentralizedExchange);

  SymbolId dexSym = reg.registerSymbol(dexId, "WETHUSDC");
  SymbolId cexSym = reg.registerSymbol(cexId, "BTCUSDT");

  EXPECT_EQ(reg.venueTypeForSymbol(dexSym), VenueType::AmmDex);
  EXPECT_EQ(reg.venueTypeForSymbol(cexSym), VenueType::CentralizedExchange);

  // An unknown symbol defaults to CentralizedExchange.
  EXPECT_EQ(reg.venueTypeForSymbol(9999), VenueType::CentralizedExchange);
}

}  // namespace
