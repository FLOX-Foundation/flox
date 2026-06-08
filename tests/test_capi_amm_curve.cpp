/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace
{
std::string amountOut(FloxCurveHandle c, size_t i, size_t j, const char* amountIn)
{
  char out[96] = {0};
  EXPECT_EQ(flox_curve_amount_out(c, i, j, amountIn, out, sizeof(out)), 1);
  return out;
}
std::string balance(FloxCurveHandle c, size_t i)
{
  char out[96] = {0};
  EXPECT_EQ(flox_curve_balance(c, i, out, sizeof(out)), 1);
  return out;
}
}  // namespace

TEST(CapiAmmCurveTest, ConstantProductPricesToTheWei)
{
  // Uniswap v2 (997/1000), reserves 1000 / 2000 (18 decimals).
  FloxCurveHandle c = flox_curve_constant_product("1000000000000000000000",
                                                  "2000000000000000000000", 997, 1000);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(flox_curve_token_count(c), 2u);
  // getAmountsOut for 1 token0 in.
  EXPECT_EQ(amountOut(c, 0, 1, "1000000000000000000"), "1992013962079806432");
  EXPECT_EQ(balance(c, 0), "1000000000000000000000");

  // apply_swap moves the pool; a clone is independent.
  FloxCurveHandle clone = flox_curve_clone(c);
  char out[96] = {0};
  EXPECT_EQ(flox_curve_apply_swap(c, 0, 1, "1000000000000000000", out, sizeof(out)), 1);
  EXPECT_EQ(balance(c, 0), "1001000000000000000000");      // reserve grew by the input
  EXPECT_EQ(balance(clone, 0), "1000000000000000000000");  // clone untouched
  flox_curve_destroy(clone);
  flox_curve_destroy(c);
}

TEST(CapiAmmCurveTest, UniswapV3InRangeMatchesQuoter)
{
  // The live USDC/WETH 0.05% pool snapshot; 1000 USDC -> WETH, in range (no ticks).
  FloxCurveHandle c = flox_curve_uniswap_v3("1959100328691929984878240664321702",
                                            "2580696918646962643", 500, nullptr, nullptr, 0);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(amountOut(c, 0, 1, "1000000000"), "611128907033491490");
  flox_curve_destroy(c);
}

TEST(CapiAmmCurveTest, ClmmStateReadBack)
{
  FloxCurveHandle c = flox_curve_uniswap_v3("1959100328691929984878240664321702",
                                            "2580696918646962643", 500, nullptr, nullptr, 0);
  ASSERT_NE(c, nullptr);
  char out[96] = {0};
  EXPECT_EQ(flox_curve_sqrt_price(c, out, sizeof(out)), 1);
  EXPECT_STREQ(out, "1959100328691929984878240664321702");
  EXPECT_EQ(flox_curve_liquidity(c, out, sizeof(out)), 1);
  EXPECT_STREQ(out, "2580696918646962643");
  flox_curve_destroy(c);

  // A non-CLMM pool reports "0" and returns 0 so the caller can branch.
  FloxCurveHandle cp = flox_curve_constant_product("1000", "2000", 997, 1000);
  EXPECT_EQ(flox_curve_sqrt_price(cp, out, sizeof(out)), 0);
  EXPECT_STREQ(out, "0");
  EXPECT_EQ(flox_curve_liquidity(cp, out, sizeof(out)), 0);
  flox_curve_destroy(cp);
}

TEST(CapiAmmCurveTest, RaydiumCpConstructsAndPrices)
{
  // Raydium CP, 0.25% trade fee, no creator fee.
  FloxCurveHandle c = flox_curve_raydium_cp("13831187668587", "13771991024747", 2500, 0, 1);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(flox_curve_token_count(c), 2u);
  const std::string out = amountOut(c, 0, 1, "1000000000");
  EXPECT_FALSE(out.empty());
  EXPECT_NE(out, "0");
  flox_curve_destroy(c);
}

TEST(CapiAmmCurveTest, RejectsBadParamsAndNullHandle)
{
  EXPECT_EQ(flox_curve_constant_product("not a number", "1", 997, 1000), nullptr);
  EXPECT_EQ(flox_curve_token_count(nullptr), 0u);
  char out[96] = {0};
  EXPECT_EQ(flox_curve_amount_out(nullptr, 0, 1, "1", out, sizeof(out)), 0);
  flox_curve_destroy(nullptr);  // no-op, must not crash
}
