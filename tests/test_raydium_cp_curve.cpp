/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/raydium_cp_curve.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

u256 U(uint64_t v) { return u256(v); }

// The Raydium constant-product test vectors, no fee, straight from the program's
// own unit tests (constant_product_swap_rounding): (input, inVault, outVault) ->
// output. The swap reproduces them to the lamport.
TEST(RaydiumCpCurveTest, ProgramTestVectorsNoFee)
{
  struct V
  {
    uint64_t in, inVault, outVault, out;
  };
  const V vs[] = {
      {10, 4000000, 70000000000, 174999},
      {20, 29980, 10000, 6},
      {19, 29980, 10000, 6},
      {18, 29980, 10000, 6},
      {10, 20000, 30000, 14},
      {10, 19991, 30000, 14},
      {10, 19990, 30000, 15},
      {100, 60000, 30000, 49},
      {99, 60000, 30000, 49},
      {98, 60000, 30000, 48},
  };
  for (const V& v : vs)
  {
    RaydiumCpCurve pool(U(v.inVault), U(v.outVault), /*tradeFeeRate*/ 0);
    EXPECT_EQ(pool.amountOut(0, 1, U(v.in)).toDec(), std::to_string(v.out))
        << "in=" << v.in << " inVault=" << v.inVault << " outVault=" << v.outVault;
  }
}

// With the 0.25% trade fee: net = input - ceil(input * 2500 / 1e6), then the
// constant product on the net. For 1,000,000 in: fee = ceil(2500) = 2500, net =
// 997500. The expected value goes through mulDiv, the same wide-intermediate
// primitive the curve itself now uses -- not the plain `*`/`/` the curve used
// to use, which would tautologically agree with a bug in the curve at large
// reserves (see OverflowingReservesUseWideIntermediate below for the case
// where that distinction actually matters).
TEST(RaydiumCpCurveTest, TradeFeeDeductedCeil)
{
  RaydiumCpCurve pool(u256::fromDec("1000000000000"), u256::fromDec("2000000000000"), 2500);
  const u256 in = u256::fromDec("1000000");
  // net = 1000000 - 2500 = 997500; out = 997500 * 2e12 / (1e12 + 997500).
  const u256 net = u256::fromDec("997500");
  const u256 expected =
      mulDiv(net, u256::fromDec("2000000000000"), u256::fromDec("1000000000000") + net);
  EXPECT_EQ(pool.amountOut(0, 1, in).toDec(), expected.toDec());
  EXPECT_FALSE(pool.amountOut(0, 1, in).isZero());
}

// applySwap moves the reserves the way the program's SwapResult does: the input
// reserve grows by the input net of fees (the fee is set aside, not added to the
// swappable reserve), and the output reserve falls by the swapped amount.
TEST(RaydiumCpCurveTest, ApplySwapReservesNetOfFee)
{
  RaydiumCpCurve pool(U(1000000), U(1000000), 2500);
  EXPECT_EQ(pool.tokenCount(), 2u);
  // fee = ceil(10000 * 2500 / 1e6) = ceil(25) = 25; net = 9975.
  const u256 net = U(9975);
  const u256 out = mulDiv(net, U(1000000), U(1000000) + net);
  auto clone = pool.clone();
  EXPECT_EQ(pool.amountOut(0, 1, U(10000)).toDec(), out.toDec());
  EXPECT_EQ(pool.applySwap(0, 1, U(10000)).toDec(), out.toDec());
  EXPECT_EQ(pool.balances()[0].toDec(), U(1000000 + 9975).toDec());  // +net, not +10000
  EXPECT_EQ(pool.balances()[1].toDec(), (U(1000000) - out).toDec());
  EXPECT_EQ(clone->balances()[0].toDec(), "1000000");  // clone untouched
}

// Creator fee on input (the default): trade and creator rates are summed and
// removed from the input in one ceil-div; the user gets the full swapped output.
TEST(RaydiumCpCurveTest, CreatorFeeOnInput)
{
  RaydiumCpCurve pool(u256::fromDec("1000000000000"), u256::fromDec("1000000000000"),
                      /*trade*/ 2500, /*creator*/ 1000, /*creatorFeeOnInput*/ true);
  const u256 in = u256::fromDec("1000000");
  // one ceil over the summed rate: fee = ceil(1000000 * 3500 / 1e6) = 3500.
  const u256 net = in - u256::fromDec("3500");
  const u256 expected =
      mulDiv(net, u256::fromDec("1000000000000"), u256::fromDec("1000000000000") + net);
  EXPECT_EQ(pool.amountOut(0, 1, in).toDec(), expected.toDec());
}

// Creator fee on output: only the trade rate comes off the input; a ceil-div
// creator fee then comes off the swapped output, so the user receives less than
// the reserve releases.
TEST(RaydiumCpCurveTest, CreatorFeeOnOutput)
{
  RaydiumCpCurve pool(u256::fromDec("1000000000000"), u256::fromDec("1000000000000"),
                      /*trade*/ 2500, /*creator*/ 1000, /*creatorFeeOnInput*/ false);
  const u256 in = u256::fromDec("1000000");
  const u256 net = in - u256::fromDec("2500");  // only the trade fee off input
  const u256 poolOut =
      mulDiv(net, u256::fromDec("1000000000000"), u256::fromDec("1000000000000") + net);
  const u256 denom = u256::pow10(6);
  const u256 creatorFee = (poolOut * u256(1000) + denom - u256(1)) / denom;  // ceil
  const u256 userOut = poolOut - creatorFee;
  EXPECT_EQ(pool.amountOut(0, 1, in).toDec(), userOut.toDec());

  // The reserve falls by the full swapped amount, not the user's net.
  pool.applySwap(0, 1, in);
  EXPECT_EQ(pool.balances()[1].toDec(), (u256::fromDec("1000000000000") - poolOut).toDec());
}

// At reserves this large, `netIn * outVault` alone exceeds 2^256 well before
// the division ever runs (261 bits here), so a plain operator* wraps mod 2^256
// silently (release builds have no assert, see u256.h) instead of computing
// the real quotient. The expected value below is computed independently in
// Python's arbitrary-precision integers, not through any u256 code path, so it
// cannot tautologically agree with a wraparound bug the way reusing the
// curve's own `*`/`/` expression would. Real Solana reserves (u64 lamports)
// never reach this regime -- this is a synthetic pool sized to demonstrate the
// class of bug, matching the mechanism the audit's independent Python oracle
// confirmed bit-for-bit (93.98% / 99.86% understated output at similar sizes).
TEST(RaydiumCpCurveTest, OverflowingReservesUseWideIntermediate)
{
  // inVault = 2^100, outVault = 2^200, in = 2^60 (no fee, to isolate the
  // overflow from the fee math already covered above).
  RaydiumCpCurve pool(u256::fromDec("1267650600228229401496703205376"),
                      u256::fromDec("1606938044258990275541962092341162602522202993782792835301376"),
                      /*tradeFeeRate*/ 0);
  const u256 in = u256::fromDec("1152921504606846976");  // 2^60

  // netIn * outVault = 2^60 * 2^200 = 2^260, which overflows 2^256; wrapped
  // mod 2^256 it happens to come out to exactly 0 (2^260 is itself a multiple
  // of 2^256), so the pre-fix code (plain `*` then `/`) returned 0 for a swap
  // that should return almost the entire output vault. mulDiv's 512-bit
  // intermediate never truncates the product before dividing.
  const u256 out = pool.amountOut(0, 1, in);
  EXPECT_EQ(out.toDec(), "1461501637329573690207901125769198826125315276800");
  EXPECT_FALSE(out.isZero());
}

TEST(RaydiumCpCurveTest, Clone)
{
  RaydiumCpCurve pool(U(1000000), U(1000000), 2500);
  auto clone = pool.clone();
  pool.applySwap(0, 1, U(10000));
  EXPECT_EQ(clone->balances()[0].toDec(), "1000000");
  EXPECT_EQ(clone->balances()[1].toDec(), "1000000");
}

}  // namespace
