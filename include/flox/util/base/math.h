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

#if !defined(__SIZEOF_INT128__)
#error "__uint128_t not supported on current compiler/target"
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
