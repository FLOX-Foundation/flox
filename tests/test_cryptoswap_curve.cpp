/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/cryptoswap_curve.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

namespace
{

// tricrypto2: USDT(6) / WBTC(8) / WETH(18), PRECISIONS [1e12, 1e10, 1]. A
// snapshot of the live pool; the expected outputs are the contract's integer
// get_dy, which this transcription reproduces to 0 wei (verified against the
// live pool across coin pairs).
CryptoswapCurve snapshotPool()
{
  std::vector<u256> bal{u256::fromDec("3142759669571"), u256::fromDec("5014135703"),
                        u256::fromDec("1929303558813993742375")};
  std::vector<u256> prec{u256::pow10(12), u256::pow10(10), u256(1)};
  std::vector<u256> scale{u256::fromDec("61120002629999063359445"),
                          u256::fromDec("1581037626232863066025")};
  return CryptoswapCurve(bal, prec, scale, /*A*/ 1707629, u256::fromDec("11809167828997"),
                         /*mid*/ u256::fromDec("3000000"), /*out*/ u256::fromDec("30000000"),
                         /*feeGamma*/ u256::fromDec("500000000000000"));
}

TEST(CryptoswapCurveTest, TricryptoToTheWei)
{
  CryptoswapCurve p = snapshotPool();
  // 10,000 USDT -> WBTC
  EXPECT_EQ(p.amountOut(0, 1, u256::fromDec("10000000000")).toDec(), "15956384");
  // 50,000 USDT -> WETH
  EXPECT_EQ(p.amountOut(0, 2, u256::fromDec("50000000000")).toDec(), "30297726093295635143");
  // 3 WETH -> WBTC
  EXPECT_EQ(p.amountOut(2, 1, u256::fromDec("3000000000000000000")).toDec(), "7770790");
  // 2 WBTC -> USDT
  EXPECT_EQ(p.amountOut(1, 0, u256::fromDec("200000000")).toDec(), "120504306349");
}

TEST(CryptoswapCurveTest, ApplySwapMovesBalances)
{
  CryptoswapCurve p = snapshotPool();
  const u256 dx = u256::fromDec("10000000000");  // 10k USDT
  const u256 out = p.amountOut(0, 1, dx);
  EXPECT_EQ(p.applySwap(0, 1, dx).toDec(), out.toDec());
  EXPECT_EQ(p.balances()[0].toDec(), "3152759669571");       // USDT += 10k (1e10 wei)
  EXPECT_EQ((p.balances()[1] + out).toDec(), "5014135703");  // WBTC -= out
}

TEST(CryptoswapCurveTest, CloneIsIndependent)
{
  CryptoswapCurve p = snapshotPool();
  EXPECT_EQ(p.tokenCount(), 3u);
  auto copy = p.clone();
  copy->applySwap(0, 1, u256::fromDec("10000000000"));
  EXPECT_EQ(p.balances()[0].toDec(), "3142759669571");  // original intact
}

}  // namespace
