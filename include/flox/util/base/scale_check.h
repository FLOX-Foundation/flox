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
#include <limits>
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

}  // namespace flox
