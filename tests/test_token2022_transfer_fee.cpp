/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/token2022_transfer_fee.h"

#include "flox/backtest/constant_product_curve.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

u256 D(const char* s) { return u256::fromDec(s); }

// Matches SPL Token-2022 TransferFee::calculate_fee: ceil(amount * bps / 10000),
// capped at maximumFee, zero when the rate is zero.
TEST(Token2022TransferFeeTest, MatchesSplCalculateFee)
{
  Token2022TransferFee half{/*bps*/ 50, /*max*/ 1000000000};  // 0.5%
  EXPECT_EQ(half.fee(D("1000000")).toDec(), "5000");          // exact
  EXPECT_EQ(half.fee(D("1000001")).toDec(), "5001");          // ceil rounds up
  EXPECT_EQ(half.fee(D("1")).toDec(), "1");                   // ceil of a tiny amount
  EXPECT_EQ(half.fee(D("0")).toDec(), "0");

  Token2022TransferFee capped{50, 4000};                // same rate, low cap
  EXPECT_EQ(capped.fee(D("1000000")).toDec(), "4000");  // min(5000, 4000)
  EXPECT_EQ(capped.afterFee(D("1000000")).toDec(), "996000");

  Token2022TransferFee none{};  // classic SPL mint
  EXPECT_EQ(none.fee(D("1000000")).toDec(), "0");
  EXPECT_EQ(none.afterFee(D("1000000")).toDec(), "1000000");
}

// The fill composes with any curve: input net of its fee feeds the pool, the user
// gets the output net of its fee.
TEST(Token2022TransferFeeTest, ComposesWithCurve)
{
  ConstantProductCurve pool(D("1000000000000"), D("2000000000000"), 997, 1000);
  const u256 plain = pool.amountOut(0, 1, D("1000000"));

  Token2022TransferFee feeIn{30, 1000000000};   // 0.3% on the input token
  Token2022TransferFee feeOut{30, 1000000000};  // 0.3% on the output token

  // Manual composition for the expectation.
  const u256 netIn = D("1000000") - D("3000");  // ceil(1000000*30/10000)=3000
  const u256 grossOut = pool.amountOut(0, 1, netIn);
  const u256 expected = grossOut - feeOut.fee(grossOut);

  const u256 got = amountOutWithTransferFees(pool, 0, 1, D("1000000"), feeIn, feeOut);
  EXPECT_EQ(got.toDec(), expected.toDec());
  EXPECT_TRUE(got < plain);  // transfer fees reduce the fill

  // With no transfer fees it is exactly the curve output.
  EXPECT_EQ(amountOutWithTransferFees(pool, 0, 1, D("1000000"), {}, {}).toDec(), plain.toDec());
}

}  // namespace
