/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/util/base/math.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

using flox::math::make_fastdiv64;
using flox::math::sdiv_ceil;
using flox::math::sdiv_floor;
using flox::math::sdiv_round_nearest;
using flox::math::udiv_fast;

namespace
{

const std::vector<uint64_t> kProbes = {0,
                                       1,
                                       2,
                                       3,
                                       4,
                                       5,
                                       7,
                                       9,
                                       15,
                                       16,
                                       17,
                                       63,
                                       64,
                                       100,
                                       999,
                                       1000,
                                       100000,
                                       1000000,
                                       999999999,
                                       1ull << 32,
                                       (1ull << 32) + 1,
                                       1ull << 62,
                                       (1ull << 63) - 1};

}  // namespace

// A divisor of 1 or 2 pushes ceil(2^65 / d) to 2^65 and 2^64, neither of which
// fits a 64-bit magic. Both truncated to zero, and the single correction step
// then answered 1 for every numerator.
TEST(FastDiv64, SmallDivisorsMatchNativeDivision)
{
  for (uint64_t d = 1; d <= 130; ++d)
  {
    const auto fd = make_fastdiv64(d);
    for (uint64_t n : kProbes)
    {
      EXPECT_EQ(udiv_fast(n, fd), n / d) << "d=" << d << " n=" << n;
    }
  }
}

TEST(FastDiv64, PowerOfTwoDivisorsMatchNativeDivision)
{
  for (unsigned s = 0; s < 63; ++s)
  {
    const uint64_t d = 1ull << s;
    const auto fd = make_fastdiv64(d);
    for (uint64_t n : kProbes)
    {
      EXPECT_EQ(udiv_fast(n, fd), n / d) << "d=" << d << " n=" << n;
    }
  }
}

TEST(FastDiv64, LargeDivisorsMatchNativeDivision)
{
  for (uint64_t d : {1000000ull, 100000000ull, (1ull << 40), (1ull << 62), (1ull << 63) - 1})
  {
    const auto fd = make_fastdiv64(d);
    for (uint64_t n : kProbes)
    {
      EXPECT_EQ(udiv_fast(n, fd), n / d) << "d=" << d << " n=" << n;
    }
  }
}

// The magic is a ceiling, so the estimate can also land one above the true
// quotient. The remainder was computed as an unsigned n - q*d, which wrapped in
// that case and pushed the answer two too high.
TEST(FastDiv64, RandomNumeratorsMatchNativeDivision)
{
  std::mt19937_64 rng(0xB05C0DEull);
  std::uniform_int_distribution<uint64_t> num(0, (1ull << 62));

  for (uint64_t d = 1; d <= 24; ++d)
  {
    const auto fd = make_fastdiv64(d);
    for (int i = 0; i < 2000; ++i)
    {
      const uint64_t n = num(rng);
      ASSERT_EQ(udiv_fast(n, fd), n / d) << "d=" << d << " n=" << n;
    }
  }
}

TEST(FastDiv64, RandomDivisorsAndNumeratorsMatchNativeDivision)
{
  std::mt19937_64 rng(0xFA57D1Full);
  std::uniform_int_distribution<uint64_t> num(0, UINT64_MAX);
  std::uniform_int_distribution<uint64_t> den(1, UINT64_MAX >> 1);

  for (int i = 0; i < 200000; ++i)
  {
    const uint64_t d = den(rng);
    const uint64_t n = num(rng);
    const auto fd = make_fastdiv64(d);
    ASSERT_EQ(udiv_fast(n, fd), n / d) << "d=" << d << " n=" << n;
  }
}

// A price scale of 1e8 makes these the divisors the order book actually builds
// from a tick size.
TEST(FastDiv64, PriceScaleDivisorsMatchNativeDivision)
{
  std::mt19937_64 rng(0x71CC5175ull);
  std::uniform_int_distribution<uint64_t> price(0, 1000000ull * 100000000ull);

  for (uint64_t d : {uint64_t{1}, uint64_t{2}, uint64_t{3}, uint64_t{5}, uint64_t{10},
                     uint64_t{100}, uint64_t{1000}, uint64_t{1000000}, uint64_t{25000000},
                     uint64_t{100000000}})
  {
    const auto fd = make_fastdiv64(d);
    for (int i = 0; i < 20000; ++i)
    {
      const uint64_t n = price(rng);
      ASSERT_EQ(udiv_fast(n, fd), n / d) << "d=" << d << " n=" << n;
    }
  }
}

// Tick indexing runs through sdiv_round_nearest, so a unit tick size has to
// round-trip every raw price unchanged.
TEST(FastDiv64, RoundNearestWithUnitDivisorIsIdentity)
{
  const auto fd = make_fastdiv64(1);
  for (int64_t n : {int64_t{0}, int64_t{1}, int64_t{1000}, int64_t{100000},
                    int64_t{999999999999}})
  {
    EXPECT_EQ(sdiv_round_nearest(n, fd), n) << "n=" << n;
  }
}

TEST(FastDiv64, RoundNearestRoundsHalfAwayFromZero)
{
  const auto fd = make_fastdiv64(10);
  EXPECT_EQ(sdiv_round_nearest(24, fd), 2);
  EXPECT_EQ(sdiv_round_nearest(25, fd), 3);
  EXPECT_EQ(sdiv_round_nearest(26, fd), 3);

  const auto fd2 = make_fastdiv64(2);
  EXPECT_EQ(sdiv_round_nearest(4, fd2), 2);
  EXPECT_EQ(sdiv_round_nearest(5, fd2), 3);
  EXPECT_EQ(sdiv_round_nearest(100000, fd2), 50000);
}

// Negative numerators came back as a huge positive number: the magnitude was
// reinterpreted as unsigned before the divide and never given its sign back.
TEST(FastDiv64, RoundNearestHandlesNegativeNumerators)
{
  for (uint64_t d : {uint64_t{1}, uint64_t{2}, uint64_t{3}, uint64_t{5}, uint64_t{10},
                     uint64_t{1000000}})
  {
    const auto fd = make_fastdiv64(d);
    for (int64_t n : {int64_t{-1}, int64_t{-4}, int64_t{-25}, int64_t{-1000},
                      int64_t{-999999999}})
    {
      EXPECT_EQ(sdiv_round_nearest(n, fd), -sdiv_round_nearest(-n, fd))
          << "d=" << d << " n=" << n;
    }
  }
}

// The conservative tick snapping an order book stores quotes with. Checked
// against the language's own truncating division, corrected to floor and to
// ceiling, over every probe divisor: a power of two takes the shift path and
// everything else the reciprocal path, and both have to agree.
TEST(FastDiv64, FloorAndCeilMatchReferenceDivision)
{
  for (uint64_t d : kProbes)
  {
    if (d == 0)
    {
      continue;
    }
    const auto fd = make_fastdiv64(d);
    const int64_t sd = static_cast<int64_t>(d);
    if (sd <= 0)
    {
      continue;  // a divisor past int64 has no signed reference to compare to
    }
    for (int64_t n : {int64_t{0}, int64_t{1}, int64_t{2}, int64_t{7}, int64_t{999},
                      int64_t{1000000}, int64_t{999999999999}, int64_t{-1}, int64_t{-2},
                      int64_t{-7}, int64_t{-999}, int64_t{-1000000},
                      int64_t{-999999999999}})
    {
      const int64_t trunc = n / sd;
      const int64_t rem = n % sd;
      const int64_t expectedFloor = (rem != 0 && ((n < 0) != (sd < 0))) ? trunc - 1 : trunc;
      const int64_t expectedCeil = (rem != 0 && ((n < 0) == (sd < 0))) ? trunc + 1 : trunc;
      EXPECT_EQ(sdiv_floor(n, fd), expectedFloor) << "d=" << d << " n=" << n;
      EXPECT_EQ(sdiv_ceil(n, fd), expectedCeil) << "d=" << d << " n=" << n;
    }
  }
}

// The property the order book depends on, stated directly: the floor never
// prices above the quote and the ceiling never below it, and a value already
// on a multiple of the divisor is left alone by both.
TEST(FastDiv64, FloorAndCeilBracketTheQuote)
{
  for (uint64_t d : {uint64_t{1}, uint64_t{2}, uint64_t{1000000}, uint64_t{100000000}})
  {
    const auto fd = make_fastdiv64(d);
    const int64_t sd = static_cast<int64_t>(d);
    for (int64_t base : {int64_t{0}, int64_t{5}, int64_t{-5}})
    {
      for (int64_t offset = 0; offset < sd && offset < 16; ++offset)
      {
        const int64_t n = base * sd + offset;
        EXPECT_LE(sdiv_floor(n, fd) * sd, n) << "d=" << d << " n=" << n;
        EXPECT_GE(sdiv_ceil(n, fd) * sd, n) << "d=" << d << " n=" << n;
        if (offset == 0)
        {
          EXPECT_EQ(sdiv_floor(n, fd), base);
          EXPECT_EQ(sdiv_ceil(n, fd), base);
        }
      }
    }
  }
}
