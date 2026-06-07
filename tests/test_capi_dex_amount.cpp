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

namespace
{
// The largest u256 (2^256 - 1) and a large i256, the values a binding round-trip
// has to survive losslessly.
constexpr const char* kU256Max =
    "115792089237316195423570985008687907853269984665640564039457584007913129639935";
}  // namespace

TEST(CapiDexAmountTest, U256RoundTripsLargeValue)
{
  char out[96] = {0};
  EXPECT_EQ(flox_u256_roundtrip(kU256Max, out, sizeof(out)), 1);
  EXPECT_STREQ(out, kU256Max);

  EXPECT_EQ(flox_u256_roundtrip("0", out, sizeof(out)), 1);
  EXPECT_STREQ(out, "0");

  // Leading zeros canonicalise away.
  EXPECT_EQ(flox_u256_roundtrip("007", out, sizeof(out)), 1);
  EXPECT_STREQ(out, "7");
}

TEST(CapiDexAmountTest, I256CarriesSign)
{
  char out[96] = {0};
  EXPECT_EQ(flox_i256_roundtrip("-500000000000000000", out, sizeof(out)), 1);
  EXPECT_STREQ(out, "-500000000000000000");

  EXPECT_EQ(flox_i256_roundtrip("42", out, sizeof(out)), 1);
  EXPECT_STREQ(out, "42");

  // Negative zero canonicalises to "0".
  EXPECT_EQ(flox_i256_roundtrip("-0", out, sizeof(out)), 1);
  EXPECT_STREQ(out, "0");
}

TEST(CapiDexAmountTest, FromHexMatchesDecimal)
{
  char out[96] = {0};
  EXPECT_EQ(flox_u256_from_hex("0xff", out, sizeof(out)), 1);
  EXPECT_STREQ(out, "255");

  // Without the 0x prefix.
  EXPECT_EQ(flox_u256_from_hex("100", out, sizeof(out)), 1);
  EXPECT_STREQ(out, "256");
}

TEST(CapiDexAmountTest, RejectsBadInputAndSmallBuffer)
{
  char out[96] = {0};
  EXPECT_EQ(flox_u256_roundtrip("not a number", out, sizeof(out)), 0);
  EXPECT_EQ(flox_u256_roundtrip(nullptr, out, sizeof(out)), 0);

  // A buffer too small for the value returns 0 rather than truncating.
  char tiny[4] = {0};
  EXPECT_EQ(flox_u256_roundtrip(kU256Max, tiny, sizeof(tiny)), 0);
}

TEST(CapiDexAmountTest, LimbWordsRoundTrip)
{
  // The little-endian limb form a Node BigInt round-trips through.
  uint64_t words[4] = {0, 0, 0, 0};
  ASSERT_EQ(flox_u256_to_words(kU256Max, words), 1);
  for (uint64_t w : words)
  {
    EXPECT_EQ(w, ~uint64_t{0});  // 2^256 - 1 is all limbs set
  }

  char out[96] = {0};
  ASSERT_EQ(flox_u256_from_words(words, out, sizeof(out)), 1);
  EXPECT_STREQ(out, kU256Max);

  // 2^64: limb 1 = 1, the rest 0 (little-endian).
  uint64_t w2[4] = {0, 0, 0, 0};
  ASSERT_EQ(flox_u256_to_words("18446744073709551616", w2), 1);
  EXPECT_EQ(w2[0], 0u);
  EXPECT_EQ(w2[1], 1u);
  EXPECT_EQ(w2[2], 0u);
  EXPECT_EQ(w2[3], 0u);
}
