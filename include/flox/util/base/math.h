/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <bit>
#include <cassert>
#include <cstdint>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace flox::math
{
inline constexpr double EPS_DOUBLE = 1e-12;

inline constexpr double EPS_PRICE = 1e-9;
inline constexpr double EPS_QTY = 1e-12;

struct FastDiv64
{
  uint64_t d;     // divisor; must be > 0, see make_fastdiv64
  uint64_t m;     // magic (high 64 bits of reciprocal); unused when pow2Shift >= 0
  unsigned k;     // extra shift (0 or 1 is enough for 64-bit)
  int pow2Shift;  // log2(d) when d is a power of two, otherwise -1
};

// A power-of-two divisor is a plain right shift, and routing it that way is
// what keeps 1 and 2 working at all: their magic, ceil(2^(64+k)/d), needs 65
// and 64 bits, truncated to zero in a uint64_t, and the single correction step
// then answered 1 for every numerator. A raw tick size of 1 or 2 is ordinary
// for sub-cent pairs, so this is a live path, not a corner.
// A zero divisor lands here too and is treated as a shift of 0, which keeps the
// result defined (it answers n) instead of dividing by zero while building the
// reciprocal. That is a fallback, not a guard: callers still have to reject a
// zero divisor themselves, which is what NLevelOrderBook's constructor does.
static inline int fastdiv_pow2_shift(uint64_t d)
{
  if (d <= 1)
  {
    return 0;
  }
  if ((d & (d - 1)) != 0)
  {
    return -1;
  }
  return std::countr_zero(d);
}

// Use __uint128_t only on non-Windows platforms with GCC/Clang
// clang-cl on Windows defines __SIZEOF_INT128__ but lacks runtime support (__udivti3)
#if defined(__SIZEOF_INT128__) && !defined(_WIN32)

// Build reciprocal: m = ceil( 2^(64+k) / d )
static inline FastDiv64 make_fastdiv64(uint64_t d, unsigned k = 1)
{
  // Debug-only: the assert compiles out under NDEBUG, which is how this
  // library ships. A caller that can receive a zero divisor has to check it.
  assert(d > 0 && "make_fastdiv64: divisor must be positive");

  FastDiv64 fd;
  fd.d = d;
  fd.m = 0;
  fd.k = k;
  fd.pow2Shift = fastdiv_pow2_shift(d);

  if (fd.pow2Shift >= 0)
  {
    return fd;
  }

  __uint128_t one = (__uint128_t)1;
  __uint128_t M = ((one << (64 + k)) + d - 1) / d;  // ceil
  fd.m = (uint64_t)M;
  return fd;
}

// Unsigned floor(n / d) using magic; exact after a two-sided correction.
//
// The estimate lands within one of the true quotient in either direction. The
// old code corrected only upwards and computed the remainder as an unsigned
// n - q*d, so an over-estimate wrapped that subtraction to a huge value, the
// r >= d test fired, and the quotient came back two too high. Multiplying back
// in 128 bits and comparing against n first catches both directions.
static inline uint64_t udiv_fast(uint64_t n, const FastDiv64& fd)
{
  if (fd.pow2Shift >= 0)
  {
    return n >> fd.pow2Shift;
  }

  __uint128_t prod = (__uint128_t)n * fd.m;
  uint64_t q = (uint64_t)(prod >> (64 + fd.k));

  __uint128_t back = (__uint128_t)q * fd.d;
  if (back > (__uint128_t)n)
  {
    --q;
    back -= fd.d;
  }
  if ((__uint128_t)n - back >= (__uint128_t)fd.d)
  {
    ++q;
  }

  return q;
}

#elif defined(_WIN32)

// Windows implementation (MSVC and clang-cl)
// Note: clang-cl has __uint128_t for multiplication but lacks __udivti3 for division
static inline FastDiv64 make_fastdiv64(uint64_t d, unsigned k = 1)
{
  // Debug-only: the assert compiles out under NDEBUG, which is how this
  // library ships. A caller that can receive a zero divisor has to check it.
  assert(d > 0 && "make_fastdiv64: divisor must be positive");

  // Compute ceil(2^(64+k) / d) using 128-bit arithmetic emulation
  // For k=1: M = ceil(2^65 / d)
  FastDiv64 fd;
  fd.d = d;
  fd.k = k;
  fd.m = 0;
  fd.pow2Shift = fastdiv_pow2_shift(d);

  if (fd.pow2Shift >= 0)
  {
    return fd;
  }

  // 2^64 / d gives us the base, then we need to shift and add for ceiling
  uint64_t q = ~0ULL / d;  // floor(2^64-1 / d)
  uint64_t r = ~0ULL % d + 1;
  if (r == d)
  {
    q++;
    r = 0;
  }
  // Now q = floor(2^64 / d), r = 2^64 mod d

  // Shift left by k and compute ceiling
  for (unsigned i = 0; i < k; ++i)
  {
    q = (q << 1) | (r >= (d - r) ? 1 : 0);
    r = (r << 1);
    if (r >= d)
    {
      r -= d;
    }
  }
  if (r > 0)
  {
    q++;  // ceiling
  }

  fd.m = q;
  return fd;
}

// Unsigned floor(n / d) using magic; exact after a two-sided correction.
static inline uint64_t udiv_fast(uint64_t n, const FastDiv64& fd)
{
  if (fd.pow2Shift >= 0)
  {
    return n >> fd.pow2Shift;
  }

  uint64_t high;
#if defined(_MSC_VER) && !defined(__clang__)
  // Pure MSVC
  uint64_t low = _umul128(n, fd.m, &high);
  (void)low;
#else
  // clang-cl: use __uint128_t for multiplication (no division involved)
  __uint128_t prod = (__uint128_t)n * fd.m;
  high = (uint64_t)(prod >> 64);
#endif

  uint64_t q = high >> fd.k;

  // Two-sided correction, as in the 128-bit path above. q*d is below 2^65, so
  // the product needs one carry bit; once q has been walked down, (q-1)*d is at
  // most n and the borrow lands back inside 64 bits.
  uint64_t backHigh;
  uint64_t backLow;
#if defined(_MSC_VER) && !defined(__clang__)
  backLow = _umul128(q, fd.d, &backHigh);
#else
  __uint128_t back = (__uint128_t)q * fd.d;
  backLow = (uint64_t)back;
  backHigh = (uint64_t)(back >> 64);
#endif

  if (backHigh != 0 || backLow > n)
  {
    --q;
    backLow -= fd.d;
  }
  if (n - backLow >= fd.d)
  {
    ++q;
  }

  return q;
}

#else
#error "No 128-bit integer support available"
#endif

// Signed division with rounding to nearest, ties away from zero: q = round(n / d).
// The magnitude is divided unsigned and the sign reapplied afterwards. Feeding
// a negative numerator straight into udiv_fast reinterprets it as a huge
// unsigned value and hands back a large positive quotient instead.
static inline int64_t sdiv_round_nearest(int64_t n, const FastDiv64& fd)
{
  const uint64_t half = fd.d >> 1;
  const bool negative = n < 0;
  // -(n + 1) + 1 keeps INT64_MIN out of undefined territory.
  const uint64_t magnitude = negative ? (uint64_t)(-(n + 1)) + 1u : (uint64_t)n;
  const uint64_t q = udiv_fast(magnitude + half, fd);

  return negative ? -(int64_t)q : (int64_t)q;
}

}  // namespace flox::math
