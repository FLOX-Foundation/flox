/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

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
  uint64_t d;  // divisor (must be > 0)
  uint64_t m;  // magic (high 64 bits of reciprocal)
  unsigned k;  // extra shift (0 or 1 is enough for 64-bit)
};

// Use __uint128_t only on non-Windows platforms with GCC/Clang
// clang-cl on Windows defines __SIZEOF_INT128__ but lacks runtime support (__udivti3)
#if defined(__SIZEOF_INT128__) && !defined(_WIN32)

// Build reciprocal: m = ceil( 2^(64+k) / d )
static inline FastDiv64 make_fastdiv64(uint64_t d, unsigned k = 1)
{
  __uint128_t one = (__uint128_t)1;
  __uint128_t M = ((one << (64 + k)) + d - 1) / d;  // ceil
  FastDiv64 fd;
  fd.d = d;
  fd.m = (uint64_t)M;
  fd.k = k;
  return fd;
}

// Unsigned floor(n / d) using magic; exact with one correction.
static inline uint64_t udiv_fast(uint64_t n, const FastDiv64& fd)
{
  __uint128_t prod = (__uint128_t)n * fd.m;
  uint64_t q = (uint64_t)(prod >> (64 + fd.k));
  uint64_t r = n - q * fd.d;

  if (r >= fd.d)
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
  // Compute ceil(2^(64+k) / d) using 128-bit arithmetic emulation
  // For k=1: M = ceil(2^65 / d)
  FastDiv64 fd;
  fd.d = d;
  fd.k = k;

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

// Unsigned floor(n / d) using magic; exact with one correction.
static inline uint64_t udiv_fast(uint64_t n, const FastDiv64& fd)
{
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
  uint64_t r = n - q * fd.d;

  if (r >= fd.d)
  {
    ++q;
  }

  return q;
}

#else
#error "No 128-bit integer support available"
#endif

// Signed division with rounding to nearest: q = round(n / d)
static inline int64_t sdiv_round_nearest(int64_t n, const FastDiv64& fd)
{
  const int64_t half = (int64_t)(fd.d >> 1);
  int64_t nadj = (n >= 0) ? (n + half) : (n - half);
  uint64_t u = (uint64_t)nadj;
  uint64_t q = udiv_fast(u, fd);

  return (int64_t)q;
}

}  // namespace flox::math
