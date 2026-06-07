/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/concentrated_liquidity_curve.h"
#include "flox/backtest/constant_product_curve.h"
#include "flox/connector/amm_dex_connector.h"
#include "flox/connector/pool_quote_view.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{

u256 D(const char* s) { return u256::fromDec(s); }

// A DEX-native strategy holds the pool as an IPoolQuoteView -- the exact curve, not
// the synthetic book -- and never has to know the venue is a pool. It values an LP
// position (the pool's reserves valued in quote at the marginal price) at each point
// on the timeline. This is the LP / IL accounting the synthetic book cannot express.
double lpValueInQuote(const IPoolQuoteView& pool)
{
  const std::vector<u256>& r = pool.reserves();
  // Marginal quote price of base from a tiny probe, exact through the curve.
  u256 probe = r[pool.baseIndex()] / u256(1000000);
  if (probe.isZero())
  {
    probe = u256(1);
  }
  const double base = std::stod(r[pool.baseIndex()].toDecimalString(18));
  const double quote = std::stod(r[pool.quoteIndex()].toDecimalString(18));
  const double out = std::stod(pool.quoteOut(probe, true).toDecimalString(18));
  const double in = std::stod(probe.toDecimalString(18));
  const double price = in > 0.0 ? out / in : 0.0;
  return base * price + quote;
}

// The view's quote is the curve's quote, for any size, with no book discretisation.
TEST(PoolQuoteViewTest, ExactQuoteMatchesCurveForArbitrarySize)
{
  ConstantProductCurve curve(D("1000000000000000000000"), D("2000000000000000000000"), 997, 1000);
  AmmDexConnector conn("amm", SymbolId{1}, curve, 0, 1, 18, 18, 3, D("1000000000000000000"));
  const IPoolQuoteView& view = conn;

  for (const char* size : {"1000000000000000", "5000000000000000000", "250000000000000000000"})
  {
    const u256 amountIn = D(size);
    EXPECT_EQ(view.quoteOut(amountIn, true).toDec(), curve.amountOut(0, 1, amountIn).toDec());
    EXPECT_EQ(view.quoteOut(amountIn, false).toDec(), curve.amountOut(1, 0, amountIn).toDec());
  }
}

// The view exposes the exact reserves, and they track the live curve as the pool
// moves -- so an LP-value calc reprices correctly after a swap, off one held view.
TEST(PoolQuoteViewTest, ReservesTrackLiveCurveForLpAccounting)
{
  ConstantProductCurve curve(D("1000000000000000000000"), D("2000000000000000000000"), 997, 1000);
  AmmDexConnector conn("amm", SymbolId{1}, curve, 0, 1, 18, 18, 1, D("1000000000000000000"));
  conn.setCallbacks([](const BookUpdateEvent&) {}, [](const TradeEvent&) {});
  const IPoolQuoteView& view = conn;

  EXPECT_EQ(view.reserves()[0].toDec(), curve.balances()[0].toDec());
  const double before = lpValueInQuote(view);

  // A taker buys base (quote in), moving the pool; the same held view reflects it.
  conn.onSwap(D("100000000000000000000"), false, 1000);
  EXPECT_EQ(view.reserves()[0].toDec(), curve.balances()[0].toDec());
  EXPECT_EQ(view.reserves()[1].toDec(), curve.balances()[1].toDec());
  const double after = lpValueInQuote(view);

  // Buying base raises its price, so the LP's value in quote rises (the IL setup).
  EXPECT_GT(after, before);
}

// The view sees venue-specific exact state through curve(): a concentrated pool's
// sqrt price and liquidity, which a generic reserves vector cannot name.
TEST(PoolQuoteViewTest, CurveAccessorReachesConcentratedState)
{
  ConcentratedLiquidityCurve clmm(D("1959100328691929984878240664321702"),
                                  D("2580696918646962643"), 500, {});
  AmmDexConnector conn("amm", SymbolId{2}, clmm, 0, 1, 18, 18, 1, D("1000000000000000000"));
  const IPoolQuoteView& view = conn;

  const auto* cl = dynamic_cast<const ConcentratedLiquidityCurve*>(&view.curve());
  ASSERT_NE(cl, nullptr);
  EXPECT_EQ(cl->sqrtPrice().toDec(), clmm.sqrtPrice().toDec());
  EXPECT_EQ(cl->liquidity().toDec(), clmm.liquidity().toDec());
}

// The synthetic book stays available: exposing the curve does not remove the
// book-shaped view a CEX-style strategy reuses.
TEST(PoolQuoteViewTest, SyntheticBookStillPublished)
{
  ConstantProductCurve curve(D("1000000000000000000000"), D("2000000000000000000000"), 1, 1);
  AmmDexConnector conn("amm", SymbolId{1}, curve, 0, 1, 18, 18, 3, D("1000000000000000000"));
  bool gotBook = false;
  std::size_t bidLevels = 0;
  conn.setCallbacks([&](const BookUpdateEvent& ev)
                    { gotBook = true; bidLevels = ev.update.bids.size(); },
                    [](const TradeEvent&) {});
  conn.republish();
  EXPECT_TRUE(gotBook);
  EXPECT_EQ(bidLevels, 3u);
}

}  // namespace
