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
// Per-symbol scale removes the compile-time guarantee that every
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

// --- Native 128-bit intermediate, and the switch that turns it off ---------
// Most of the fixed-point arithmetic wants a 128-bit intermediate. GCC and
// Clang have one (__int128); MSVC does not, so those builds take a software
// path written out of 64-bit halves. A path only one toolchain compiles is a
// path nobody tests, which is exactly how the hand-rolled fallback in
// common.h shipped a backtest that returned -534 where 5000 was expected.
//
// FLOX_FORCE_PORTABLE_INT128=1 compiles the native intermediate out even
// where the type exists, so the MSVC path is built and run on macOS and Linux
// too. Nothing but a test build should define it: it is slower and produces
// the same numbers. tests/CMakeLists.txt builds the fixed-point suite twice,
// once each way; mulDivI64As<Portable> and Decimal's *Path members select a
// path per call site, which is how one binary can check the two against each
// other.
#if !defined(FLOX_FORCE_PORTABLE_INT128)
#define FLOX_FORCE_PORTABLE_INT128 0
#endif

#if defined(__SIZEOF_INT128__) && !FLOX_FORCE_PORTABLE_INT128
#define FLOX_HAS_NATIVE_INT128 1
#else
#define FLOX_HAS_NATIVE_INT128 0
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

// A double that was MEANT to be a fixed-point raw: a rate an operator typed, a
// schedule expressed in basis points, a number that arrived as JSON. The
// decimal never survived the trip into binary -- 0.0003 is stored as
// 0.00029999999999999997 -- so truncating the scaled value drops the last raw
// off nearly every rate a human writes. Rounding to nearest returns the raw
// that was meant, and rounds a rate and its negation to mirror images of each
// other, which a rate that is paid in one direction and received in the other
// has to do.
constexpr int64_t roundDoubleToI64(double v) noexcept
{
  if (!(v == v))
  {
    return 0;
  }
  return narrowDoubleToI64(v < 0.0 ? v - 0.5 : v + 0.5);
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

// `constexpr` here holds only in a build where FLOX_SCALE_CHECKS is 0
// (NDEBUG, i.e. every configuration this project's CMakeLists.txt
// actually produces -- CMAKE_BUILD_TYPE is forced to Release). With
// FLOX_SCALE_CHECKS on, FLOX_SCALE_CHECK below always expands to
// `assert(false && ...)`, which no compiler can constant-evaluate, so
// the function cannot be a constant expression for any input in that
// configuration. Clang diagnoses that as a hard error by default
// (-Winvalid-constexpr) the moment such a build compiles this header;
// GCC has no equivalent diagnostic, so the project's GCC-based
// sanitizer lane (Debug, checks on) does not see it. Not live in any
// build this project's CI runs today, but real enough that a future
// Clang-based debug lane would hit it immediately.
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

// Checked int64 subtraction, the mirror of checkedAddI64 and used by
// Decimal::operator-= and the binary operator-. `a - b` on int64_t overflows
// exactly as an addition does -- an inventory accumulator walked down past
// INT64_MIN wraps to a large positive position, which reads as a long book
// where a short one is held. Detected through unsigned arithmetic (defined
// behavior) and saturated at the boundary.
constexpr int64_t checkedSubI64(int64_t a, int64_t b) noexcept
{
  const uint64_t ua = static_cast<uint64_t>(a);
  const uint64_t ub = static_cast<uint64_t>(b);
  const uint64_t udiff = ua - ub;
  // Signed overflow occurred iff the operands differ in sign and the result's
  // sign differs from the minuend's.
  const bool overflowed = static_cast<bool>(((ua ^ ub) & (ua ^ udiff)) >> 63);
  FLOX_SCALE_CHECK(!overflowed,
                   "fixed-point subtraction overflow (Decimal::operator- exceeds int64 range)");
  if (overflowed)
  {
    return a >= 0 ? (std::numeric_limits<int64_t>::max)() : (std::numeric_limits<int64_t>::min)();
  }
  return static_cast<int64_t>(udiff);
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
// Multiply two int64 and divide by an int64, exactly, saturating at the
// int64 boundary rather than wrapping. Every fixed-point operator that turns
// a price and a quantity into money goes through here.
//
// The portable implementation below is compiled ALWAYS, not only where a
// native 128-bit integer is missing. It used to live behind #else, which
// meant it was never built on any platform this project supports -- untested
// code standing by for the day it would be needed, which is the shape of
// every defect this file exists to prevent. Now it is built everywhere and
// checked against the hardware path by test_fixed_point_muldiv.
namespace detail
{
// 64x64 -> 128 through 32-bit halves.
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
    const uint64_t nextBit = (bit >= 64) ? ((hi >> (bit - 64)) & 1u) : ((lo >> bit) & 1u);
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

// The magnitudes, with the sign handled by the caller. Splitting it this way
// is what lets the hardware paths work on unsigned values without any of them
// having to get the sign right a second time -- the MSVC intrinsic path used
// to cast a signed value straight to uint64_t, so a negative volume became
// 1.8e19 and the answer was nonsense.
constexpr int64_t mulDivMagnitude(uint64_t ua, uint64_t ub, uint64_t ud, bool negative) noexcept
{
  uint64_t hi = 0;
  uint64_t lo = 0;
  umul64(ua, ub, hi, lo);

  constexpr uint64_t kMax = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
  if (hi >= ud)
  {
    FLOX_SCALE_CHECK(false, "fixed-point mulDiv overflow (quotient exceeds int64 range)");
    return negative ? (std::numeric_limits<int64_t>::min)()
                    : (std::numeric_limits<int64_t>::max)();
  }

  const uint64_t q = udiv128By64(hi, lo, ud);
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
}  // namespace detail

constexpr int64_t mulDivI64(int64_t a, int64_t b, int64_t d) noexcept
{
  if (d == 0)
  {
    const bool nonZeroProduct = (a != 0) && (b != 0);
    const bool negativeProduct = (a < 0) != (b < 0);
    return dividedByZeroI64(nonZeroProduct ? (negativeProduct ? -1 : 1) : 0);
  }
#if FLOX_HAS_NATIVE_INT128
  using i128 = __int128_t;
  return checkedNarrowI64((i128)a * (i128)b / (i128)d);
#else
  const bool negative = ((a < 0) != (b < 0)) != (d < 0);
  return detail::mulDivMagnitude(detail::absToU64(a), detail::absToU64(b), detail::absToU64(d),
                                 negative);
#endif
}

// The portable path by name, so a test on a platform that HAS a 128-bit type
// can still exercise the code a platform without one would run. Untested
// fallbacks are how a backtest came back with -534 where 5000 was expected.
constexpr int64_t mulDivI64Portable(int64_t a, int64_t b, int64_t d) noexcept
{
  if (d == 0)
  {
    const bool nonZeroProduct = (a != 0) && (b != 0);
    const bool negativeProduct = (a < 0) != (b < 0);
    return dividedByZeroI64(nonZeroProduct ? (negativeProduct ? -1 : 1) : 0);
  }
  const bool negative = ((a < 0) != (b < 0)) != (d < 0);
  return detail::mulDivMagnitude(detail::absToU64(a), detail::absToU64(b), detail::absToU64(d),
                                 negative);
}

// Checked int64 multiplication, used by Decimal::operator*(int64_t) -- the
// scalar multiply, which unlike the fixed-point one has no Scale to divide
// the product back down. A quantity raw scaled by a lot count is one
// multiplication away from the int64 ceiling. Built on the same 64x64->128
// helper the portable mulDiv uses, so there is one piece of widening
// arithmetic in this file and not two.
constexpr int64_t checkedMulI64(int64_t a, int64_t b) noexcept
{
  if (a == 0 || b == 0)
  {
    return 0;
  }
  const bool negative = (a < 0) != (b < 0);
  uint64_t hi = 0;
  uint64_t lo = 0;
  detail::umul64(detail::absToU64(a), detail::absToU64(b), hi, lo);

  constexpr uint64_t kMax = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
  const uint64_t limit = negative ? kMax + 1u : kMax;
  const bool overflowed = (hi != 0) || (lo > limit);
  FLOX_SCALE_CHECK(!overflowed,
                   "fixed-point multiplication overflow (Decimal::operator*(int64_t) exceeds int64 range)");
  if (overflowed)
  {
    return negative ? (std::numeric_limits<int64_t>::min)()
                    : (std::numeric_limits<int64_t>::max)();
  }
  if (negative)
  {
    return static_cast<int64_t>(~lo + 1u);
  }
  return static_cast<int64_t>(lo);
}

// mulDivI64 with the path chosen at the call site rather than by the
// toolchain. `mulDivI64As<false>` is whatever this build would use anyway;
// `mulDivI64As<true>` is always the software path. A test can therefore run
// both in one binary on any platform, and Decimal routes its operators
// through it so the whole class can be built either way (see
// FLOX_FORCE_PORTABLE_INT128 at the top of this file).
template <bool Portable>
constexpr int64_t mulDivI64As(int64_t a, int64_t b, int64_t d) noexcept
{
  if constexpr (Portable)
  {
    return mulDivI64Portable(a, b, d);
  }
  else
  {
    return mulDivI64(a, b, d);
  }
}

}  // namespace flox
