/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/stableswap_curve.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

// 3pool: DAI(18) / USDC(6) / USDT(6), RATES [1e18, 1e30, 1e30], A 4000, fee
// 1500000 over 1e10. Vectors computed from the contract's integer algorithm,
// which matches the live 3pool get_dy to 0 wei.
const std::vector<u256> kRates{u256::pow10(18), u256::pow10(30), u256::pow10(30)};

StableSwapCurve pool(const std::vector<u256>& bal)
{
  return StableSwapCurve(bal, kRates, 4000, u256::fromDec("1500000"));
}

TEST(StableSwapCurveTest, BalancedToTheWei)
{
  std::vector<u256> bal{u256::fromDec("50000000000000000000000000"),  // 50M DAI
                        u256::fromDec("50000000000000"),              // 50M USDC
                        u256::fromDec("50000000000000")};             // 50M USDT
  StableSwapCurve p = pool(bal);
  EXPECT_EQ(p.amountOut(0, 1, u256::fromDec("1000000000000000000000000")).toDec(), "999845000026");
  EXPECT_EQ(p.amountOut(1, 0, u256::fromDec("1000000000000")).toDec(),
            "999845000025513770802667");
  EXPECT_EQ(p.amountOut(1, 2, u256::fromDec("2000000000000")).toDec(), "1999679976177");
}

TEST(StableSwapCurveTest, ImbalancedToTheWei)
{
  std::vector<u256> bal{u256::fromDec("80000000000000000000000000"),  // 80M DAI
                        u256::fromDec("30000000000000"),              // 30M USDC
                        u256::fromDec("40000000000000")};             // 40M USDT
  StableSwapCurve p = pool(bal);
  EXPECT_EQ(p.amountOut(0, 1, u256::fromDec("1000000000000000000000000")).toDec(), "999496915909");
  EXPECT_EQ(p.amountOut(2, 0, u256::fromDec("500000000000")).toDec(),
            "500024792183608015222149");
}

TEST(StableSwapCurveTest, ApplySwapMovesBalances)
{
  std::vector<u256> bal{u256::fromDec("50000000000000000000000000"),
                        u256::fromDec("50000000000000"), u256::fromDec("50000000000000")};
  StableSwapCurve p = pool(bal);
  const u256 dx = u256::fromDec("1000000000000000000000000");
  const u256 out = p.amountOut(0, 1, dx);
  EXPECT_EQ(p.applySwap(0, 1, dx).toDec(), out.toDec());
  EXPECT_EQ((p.balances()[0]).toDec(), "51000000000000000000000000");  // DAI += 1M
  EXPECT_EQ((p.balances()[1] + out).toDec(), "50000000000000");        // USDC -= out
}

TEST(StableSwapCurveTest, CloneIsIndependent)
{
  std::vector<u256> bal{u256::fromDec("50000000000000000000000000"),
                        u256::fromDec("50000000000000"), u256::fromDec("50000000000000")};
  StableSwapCurve p = pool(bal);
  EXPECT_EQ(p.tokenCount(), 3u);
  auto copy = p.clone();
  copy->applySwap(0, 1, u256::fromDec("1000000000000000000000000"));
  EXPECT_EQ(p.balances()[0].toDec(), "50000000000000000000000000");  // original intact
}

}  // namespace
