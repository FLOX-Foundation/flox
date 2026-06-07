/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/repegging_cryptoswap_pool.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

u256 e(const char* s) { return u256::fromDec(s); }

// A live tricrypto2 snapshot. Applying one swap (50,000 USDT -> WETH, dt 300s)
// drives the full tweak_price: the EMA oracle, the xcp_profit / virtual_price
// update, and a price-scale repeg. The expected post-swap state is from the
// contract's integer tweak_price (transcription whose components -- newton_D /
// newton_y, geometric_mean / virtual_price, halfpow -- are each verified against
// the live pool).
RepeggingCryptoswapPool snapshot()
{
  return RepeggingCryptoswapPool(
      {e("3146642156353"), e("5036653109"), e("1918331947286142193477")},  // balances
      {u256::pow10(12), u256::pow10(10), u256(1)},                         // precisions
      {e("61273887361742323622141"), e("1586761157964874558441")},         // price_scale
      1707629, e("11809167828997"),                                        // A, gamma
      e("3000000"), e("30000000"), e("500000000000000"),                   // mid, out, fee_gamma
      {e("62273790044523572607131"), e("1631903369313083162152")},         // price_oracle
      {e("62193952649926661446180"), e("1630805403745550899919")},         // last_prices
      e("1139925123781020459"), e("1069963754814589039"),                  // xcp_profit, vp
      e("9276190443093409994891544"), e("6284687250757967563960"),         // D, total_supply
      e("600"), e("2000000000000"), e("490000000000000"),                  // ma_half_time, aep, adj
      false, e("300"));                                                    // not_adjusted, dt
}

TEST(RepeggingCryptoswapPoolTest, TweakPriceMatchesContract)
{
  RepeggingCryptoswapPool pool = snapshot();
  const u256 out = pool.applySwap(0, 2, e("50000000000"));  // 50k USDT -> WETH
  EXPECT_EQ(out.toDec(), "30090245653195501000");
  EXPECT_EQ(pool.priceOracle()[0].toDec(), "62250406213496417163820");
  EXPECT_EQ(pool.priceOracle()[1].toDec(), "1631581782650161141168");
  EXPECT_EQ(pool.xcpProfit().toDec(), "1139938024014649697");
  EXPECT_EQ(pool.virtualPrice().toDec(), "1069975525973047594");
  // The repeg fired: price_scale stepped toward the oracle.
  EXPECT_EQ(pool.priceScale()[0].toDec(), "61288640971209585135304");
  EXPECT_EQ(pool.priceScale()[1].toDec(), "1587438324607891849475");
}

TEST(RepeggingCryptoswapPoolTest, CloneIsIndependent)
{
  RepeggingCryptoswapPool pool = snapshot();
  auto clone = pool.clone();
  clone->applySwap(0, 2, e("50000000000"));
  // Original price scale untouched by the clone's swap.
  EXPECT_EQ(pool.priceScale()[0].toDec(), "61273887361742323622141");
}

}  // namespace
