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

}  // namespace flox
