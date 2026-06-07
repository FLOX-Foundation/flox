/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/util/int/u256.h"

#include <cstdint>

namespace flox
{

// SPL Token-2022 transfer fee, exact per the program's TransferFee::calculate_fee:
// ceil(amount * basis_points / 10000), capped at maximumFee, and zero when the rate
// is zero. The basis points and cap are the ones in effect for the current epoch
// (a Token-2022 mint carries an older and a newer TransferFee and picks by epoch);
// resolving which applies is the reader's job, this holds the chosen pair.
//
// A transfer-fee mint changes the fill a swap delivers even when the pool curve is
// exact: the pool receives the input net of its transfer fee, and the user receives
// the output net of its transfer fee. This lives at the connector boundary, where
// native amounts become engine events, and composes with any curve -- it is not
// part of the curve.
struct Token2022TransferFee
{
  uint16_t basisPoints = 0;
  uint64_t maximumFee = 0;

  // The fee withheld on transferring `amount`.
  u256 fee(const u256& amount) const
  {
    if (basisPoints == 0 || amount.isZero())
    {
      return u256(0);
    }
    const u256 denom(10000);
    const u256 raw = (amount * u256(basisPoints) + (denom - u256(1))) / denom;  // ceil
    const u256 cap(maximumFee);
    return raw < cap ? raw : cap;
  }

  // The amount that arrives after the fee is withheld.
  u256 afterFee(const u256& amount) const { return amount - fee(amount); }
};

// The fill a swap delivers when either leg is a transfer-fee mint: the pool is fed
// the input net of its fee, and the user receives the curve output net of its fee.
// feeIn / feeOut are the in-token and out-token transfer fees (a default-constructed
// one, rate zero, is a classic SPL mint and a no-op).
template <typename Curve>
inline u256 amountOutWithTransferFees(const Curve& curve, std::size_t i, std::size_t j,
                                      const u256& amountIn, const Token2022TransferFee& feeIn,
                                      const Token2022TransferFee& feeOut)
{
  const u256 netIn = feeIn.afterFee(amountIn);
  const u256 out = curve.amountOut(i, j, netIn);
  return feeOut.afterFee(out);
}

}  // namespace flox
