/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <version>

// Single point of truth for fixed-point scale safety. Two concerns live here:
//   1. Feature detection (C++26 / P2900 contracts / saturation arithmetic).
//   2. The FLOX_SCALE_CHECK guardrail and a checked int128->int64 narrowing
//      helper used by the fixed-point arithmetic.
//
// Per-symbol scale (W17-T001) removes the compile-time guarantee that every
// Price shares one scale. The guarantee is recovered at dev/CI time through
// FLOX_SCALE_CHECK; in release builds the check compiles to nothing. The
// macro spelling for contracts is still settling across toolchains, so any
// adjustment happens HERE and nowhere else.

// --- C++26 detection -------------------------------------------------------
#if __cplusplus > 202302L
#define FLOX_CXX26 1
#else
#define FLOX_CXX26 0
#endif

// --- P2900 contracts detection ---------------------------------------------
// __cpp_contracts is the SD-6 feature-test macro for contract_assert.
#if defined(__cpp_contracts) && __cpp_contracts >= 202502L
#define FLOX_HAS_CONTRACTS 1
#else
#define FLOX_HAS_CONTRACTS 0
#endif

// --- Are scale checks active? ----------------------------------------------
// On in debug / CI by default, compiled out in release. Force with
// -DFLOX_SCALE_CHECKS=0 or =1 (the experimental C++26 CI lane forces 1).
#if !defined(FLOX_SCALE_CHECKS)
#if defined(NDEBUG)
#define FLOX_SCALE_CHECKS 0
#else
#define FLOX_SCALE_CHECKS 1
#endif
#endif

#if FLOX_SCALE_CHECKS
#if FLOX_HAS_CONTRACTS
#define FLOX_SCALE_CHECK(cond, msg) contract_assert(cond)
#else
#include <cassert>
#define FLOX_SCALE_CHECK(cond, msg) assert((cond) && (msg))
#endif
#else
#define FLOX_SCALE_CHECK(cond, msg) ((void)0)
#endif

namespace flox
{

// Saturation arithmetic: native std under C++26, otherwise a clamp polyfill.
// Used for the narrowing casts in fixed-point multiply/divide so DEX-range
// values clamp to the int64 boundary instead of wrapping silently.
#if FLOX_CXX26 && defined(__cpp_lib_saturation_arithmetic)
template <typename To, typename From>
constexpr To saturate_cast(From v) noexcept
{
  return std::saturate_cast<To>(v);
}
#else
template <typename To, typename From>
constexpr To saturate_cast(From v) noexcept
{
  constexpr From lo = static_cast<From>((std::numeric_limits<To>::min)());
  constexpr From hi = static_cast<From>((std::numeric_limits<To>::max)());
  if (v < lo)
  {
    return (std::numeric_limits<To>::min)();
  }
  if (v > hi)
  {
    return (std::numeric_limits<To>::max)();
  }
  return static_cast<To>(v);
}
#endif

// Narrowing a double into a fixed-point raw. static_cast is undefined for a
// value the target cannot represent, and the two architectures this engine
// ships on disagree about what comes out: arm64 saturates toward the bound,
// x86-64 yields the sentinel. Every JSON number, every binding argument and
// every configuration file reaches fromDouble, so the conversion has to be
// defined rather than merely observed. Out of range clamps to the bound; a NaN
// has no nearest representable value at all, and zero is the only answer that
// cannot be mistaken for a real quantity.
//
// Deliberately not a FLOX_SCALE_CHECK: this runs on operator and client input,
// where the right answer is a rejection at the perimeter (see the venue's
// control API), not an abort deep inside arithmetic.
constexpr int64_t narrowDoubleToI64(double v) noexcept
{
  if (!(v == v))
  {
    return 0;
  }
  // 2^63 is exactly representable as a double; int64 max is not, so compare
  // against the power of two and clamp.
  constexpr double kUpper = 9223372036854775808.0;
  constexpr double kLower = -9223372036854775808.0;
  if (v >= kUpper)
  {
    return (std::numeric_limits<int64_t>::max)();
  }
  if (v < kLower)
  {
    return (std::numeric_limits<int64_t>::min)();
  }
  return static_cast<int64_t>(v);
}

#if defined(__SIZEOF_INT128__)
// Checked narrowing of a 128-bit intermediate down to int64. In checked
// builds an out-of-range value trips FLOX_SCALE_CHECK; in all builds it
// saturates rather than wrapping (defined behavior, not UB). This is the
// chokepoint the fixed-point operators route their narrowing through.
constexpr int64_t checkedNarrowI64(__int128_t v) noexcept
{
  FLOX_SCALE_CHECK(v >= (__int128_t)(std::numeric_limits<int64_t>::min)() &&
                       v <= (__int128_t)(std::numeric_limits<int64_t>::max)(),
                   "fixed-point narrowing overflow (scale too fine for value range)");
  return saturate_cast<int64_t>(v);
}
#endif

// A zero divisor in fixed-point division. There is no representable answer,
// and the raw machine behavior is not portable: x86 raises a hardware
// exception, arm64 returns an unspecified value without trapping at all, and
// the __int128 software path hands back whatever the runtime helper left in
// the register. That last case is the dangerous one -- dividing a filled
// volume by a zero filled quantity produced a plausible-looking execution
// price (0.0 in one build, 25.0 in another, from the same input) that then
// flowed into PnL and risk.
//
// Checked builds trip FLOX_SCALE_CHECK. Every build returns a saturated
// value, consistent with checkedNarrowI64 and checkedAddI64 above: defined
// behavior, and a number at the int64 boundary that no real price or
// quantity can be mistaken for.
namespace detail
{
inline std::atomic<uint64_t> g_divisionsByZero{0};
}  // namespace detail

// How many fixed-point divisions by zero this process has taken. A release
// build has no assertion to trip, so this counter is what a health check or a
// test reads to tell a saturated result apart from an ordinary one. Nothing
// on the hot path touches it: it only moves on the zero-divisor path.
inline uint64_t fixedPointDivisionsByZero() noexcept
{
  return detail::g_divisionsByZero.load(std::memory_order_relaxed);
}

inline void resetFixedPointDivisionsByZero() noexcept
{
  detail::g_divisionsByZero.store(0, std::memory_order_relaxed);
}

constexpr int64_t dividedByZeroI64(int64_t numerator) noexcept
{
  FLOX_SCALE_CHECK(false, "fixed-point division by zero");
  if (!std::is_constant_evaluated())
  {
    detail::g_divisionsByZero.fetch_add(1, std::memory_order_relaxed);
  }
  if (numerator == 0)
  {
    return 0;
  }
  return numerator > 0 ? (std::numeric_limits<int64_t>::max)()
                       : (std::numeric_limits<int64_t>::min)();
}

// Checked int64 addition, used by Decimal::operator+= (accumulators such as
// Bar::volume). Plain `a + b` on int64_t is signed overflow, which is
// undefined behavior in C++, not a defined wraparound -- observed in
// practice to flip sign at -O0 and to disappear (the compiler assuming
// overflow cannot happen) at higher optimization levels, so the same
// accumulation reports a different number depending on how it was built.
// This detects overflow through unsigned arithmetic (defined behavior) and
// saturates at the int64 boundary instead, consistent with checkedNarrowI64
// above. No __int128 dependency, so it is available on every toolchain this
// project targets, including MSVC.
constexpr int64_t checkedAddI64(int64_t a, int64_t b) noexcept
{
  const uint64_t ua = static_cast<uint64_t>(a);
  const uint64_t ub = static_cast<uint64_t>(b);
  const uint64_t usum = ua + ub;
  // Signed overflow occurred iff both operands have the same sign and the
  // result's sign differs from theirs.
  const bool overflowed = static_cast<bool>((~(ua ^ ub) & (ua ^ usum)) >> 63);
  FLOX_SCALE_CHECK(!overflowed, "fixed-point accumulation overflow (Decimal::operator+= exceeds int64 range)");
  if (overflowed)
  {
    return b >= 0 ? (std::numeric_limits<int64_t>::max)() : (std::numeric_limits<int64_t>::min)();
  }
  return static_cast<int64_t>(usum);
}

// Full-width multiply-then-divide over fixed-point raws. Written out as
// `(a * b) / d` in int64 the product overflows long before any of the three
// operands does: a 200 USD fee raw against a 7-unit quantity raw is
// 2e10 * 7e8 = 1.4e19, past the int64 ceiling. Signed overflow is undefined
// behaviour rather than a wrap, and what came out of it in practice was a
// NEGATIVE fee, money credited to an account for a trade that cost it. The
// product is carried at full width here and only the quotient, which does
// fit, is narrowed.
//
// A zero divisor routes through dividedByZeroI64, so it is counted and
// saturated the same way as every other fixed-point division by zero.
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
constexpr int64_t mulDivI64(int64_t a, int64_t b, int64_t d) noexcept
{
  using i128 = __int128_t;
  if (d == 0)
  {
    const bool nonZero = (a != 0) && (b != 0);
    const bool negative = (a < 0) != (b < 0);
    return dividedByZeroI64(nonZero ? (negative ? -1 : 1) : 0);
  }
  return checkedNarrowI64((i128)a * (i128)b / (i128)d);
}
#else
namespace detail
{
// 64x64 -> 128 through 32-bit halves, for toolchains with no native 128-bit
// integer type.
constexpr void umul64(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) noexcept
{
  const uint64_t aLo = a & 0xFFFFFFFFull;
  const uint64_t aHi = a >> 32;
  const uint64_t bLo = b & 0xFFFFFFFFull;
  const uint64_t bHi = b >> 32;

  const uint64_t p0 = aLo * bLo;
  const uint64_t p1 = aLo * bHi;
  const uint64_t p2 = aHi * bLo;
  const uint64_t p3 = aHi * bHi;

  const uint64_t middle = p1 + (p0 >> 32) + (p2 & 0xFFFFFFFFull);
  lo = (middle << 32) | (p0 & 0xFFFFFFFFull);
  hi = p3 + (p2 >> 32) + (middle >> 32);
}

// (hi:lo) / d by shift-subtract. The caller establishes hi < d first, so the
// quotient is known to fit in 64 bits.
constexpr uint64_t udiv128By64(uint64_t hi, uint64_t lo, uint64_t d) noexcept
{
  uint64_t quotient = 0;
  uint64_t remainder = 0;
  for (int bit = 127; bit >= 0; --bit)
  {
    const uint64_t nextBit =
        (bit >= 64) ? ((hi >> (bit - 64)) & 1u) : ((lo >> bit) & 1u);
    remainder = (remainder << 1) | nextBit;
    if (remainder >= d)
    {
      remainder -= d;
      if (bit < 64)
      {
        quotient |= (uint64_t{1} << bit);
      }
    }
  }
  return quotient;
}

constexpr uint64_t absToU64(int64_t v) noexcept
{
  return v < 0 ? (~static_cast<uint64_t>(v) + 1u) : static_cast<uint64_t>(v);
}
}  // namespace detail

constexpr int64_t mulDivI64(int64_t a, int64_t b, int64_t d) noexcept
{
  if (d == 0)
  {
    const bool nonZeroProduct = (a != 0) && (b != 0);
    const bool negativeProduct = (a < 0) != (b < 0);
    return dividedByZeroI64(nonZeroProduct ? (negativeProduct ? -1 : 1) : 0);
  }

  const bool negative = ((a < 0) != (b < 0)) != (d < 0);
  uint64_t hi = 0;
  uint64_t lo = 0;
  detail::umul64(detail::absToU64(a), detail::absToU64(b), hi, lo);
  const uint64_t ud = detail::absToU64(d);

  constexpr uint64_t kMax = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
  if (hi >= ud)
  {
    FLOX_SCALE_CHECK(false, "fixed-point mulDiv overflow (quotient exceeds int64 range)");
    return negative ? (std::numeric_limits<int64_t>::min)()
                    : (std::numeric_limits<int64_t>::max)();
  }

  const uint64_t q = detail::udiv128By64(hi, lo, ud);
  if (negative)
  {
    if (q > kMax + 1u)
    {
      FLOX_SCALE_CHECK(false, "fixed-point mulDiv overflow (quotient exceeds int64 range)");
      return (std::numeric_limits<int64_t>::min)();
    }
    return static_cast<int64_t>(~q + 1u);
  }
  if (q > kMax)
  {
    FLOX_SCALE_CHECK(false, "fixed-point mulDiv overflow (quotient exceeds int64 range)");
    return (std::numeric_limits<int64_t>::max)();
  }
  return static_cast<int64_t>(q);
}
#endif

}  // namespace flox
