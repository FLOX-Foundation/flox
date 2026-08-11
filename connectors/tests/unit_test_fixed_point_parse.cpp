/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * util::parseFixedPoint: decimal string -> fixed-point raw, no double, lenient
 * on excess precision (exchange data is authoritative).
 */

#include "flox-connectors/util/safe_parse.h"

#include <flox/common.h>

#include <gtest/gtest.h>

using namespace flox;

namespace
{
constexpr int64_t S = Price::Scale;  // 1e8

int64_t parse(std::string_view sv)
{
  int64_t out = -1;
  EXPECT_TRUE(util::parseFixedPoint(sv, S, out)) << "failed to parse: " << sv;
  return out;
}
}  // namespace

TEST(FixedPointParse, ExactValues)
{
  EXPECT_EQ(parse("0"), 0);
  EXPECT_EQ(parse("1"), S);
  EXPECT_EQ(parse("50000"), 50000LL * S);
  EXPECT_EQ(parse("50000.5"), 50000LL * S + S / 2);
  EXPECT_EQ(parse("0.00000001"), 1);  // one raw unit at 1e-8
  EXPECT_EQ(parse("100.25"), 10025LL * (S / 100));
  EXPECT_EQ(parse("-3.5"), -(3LL * S + S / 2));  // sign supported
}

TEST(FixedPointParse, MatchesFromDoubleWithoutTheDouble)
{
  // The value fromDouble would produce, computed without a double round-trip.
  int64_t raw = 0;
  ASSERT_TRUE(util::parseFixedPoint("12345.678", S, raw));
  EXPECT_EQ(Price::fromRaw(raw), Price::fromDouble(12345.678));
}

TEST(FixedPointParse, TruncatesExcessPrecision)
{
  // 9+ fractional digits: digits beyond 1e-8 are truncated toward zero, the
  // level is NOT dropped.
  int64_t raw = 0;
  ASSERT_TRUE(util::parseFixedPoint("0.123456789", S, raw));
  EXPECT_EQ(raw, 12345678);  // 0.12345678, last digit dropped
}

TEST(FixedPointParse, Rejects)
{
  int64_t out = 0;
  EXPECT_FALSE(util::parseFixedPoint("", S, out));
  EXPECT_FALSE(util::parseFixedPoint("abc", S, out));
  EXPECT_FALSE(util::parseFixedPoint("1.2.3", S, out));
  EXPECT_FALSE(util::parseFixedPoint("1e5", S, out));          // exponent unsupported
  EXPECT_FALSE(util::parseFixedPoint("12 ", S, out));          // trailing space
  EXPECT_FALSE(util::parseFixedPoint("99999999999", S, out));  // int part overflow
}
