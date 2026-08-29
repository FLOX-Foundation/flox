/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Clock domains as types.
 *
 * UnixNanos and MonoNanos used to be aliases of int64/uint64, so a
 * steady-clock reading could be assigned into a wall-clock field without a
 * sound -- and was, twice: a tape connector wrote book records 56 years adrift
 * of their own trades, and the receive path mixed three clock domains behind
 * one field. The strong types exist to turn that whole class into compile
 * errors, so the assertions that matter here are the ones that must NOT
 * compile. Runtime checks cover layout and the arithmetic that is allowed.
 */

#include "flox/util/base/time.h"

#include <gtest/gtest.h>

#include <cstring>
#include <type_traits>

using namespace flox;

// The negative half, done with requires-expressions: each names an expression
// that used to compile under the aliases and no longer may. A static_assert on
// a requires clause fails at compile time the moment someone reopens the hole,
// which is the entire budget of this test.
template <typename U, typename M>
constexpr bool kCrossAssignable = requires(U u, M m) { u = m; };
template <typename U, typename M>
constexpr bool kCrossSubtractable = requires(U u, M m) { u - m; };
template <typename T>
constexpr bool kIntAssignable = requires(T t) { t = 5; };
template <typename T>
constexpr bool kImplicitFromInt = std::is_convertible_v<int64_t, T>;
template <typename T>
constexpr bool kImplicitToInt = std::is_convertible_v<T, int64_t>;

static_assert(!kCrossAssignable<UnixNanos, SeqNanos>,
              "sequencer time is not wall time: a replayed journal re-derives "
              "the same SeqNanos at a different wall moment");
static_assert(!kCrossAssignable<SeqNanos, UnixNanos>,
              "the one legitimate crossing (capture) goes through fromRaw, named");
static_assert(!kCrossAssignable<SeqNanos, MonoNanos> && !kCrossAssignable<MonoNanos, SeqNanos>,
              "sequencer and steady never meet");
static_assert(!kCrossSubtractable<UnixNanos, SeqNanos> && !kCrossSubtractable<SeqNanos, MonoNanos>,
              "cross-domain intervals are fiction in every pairing");
static_assert(!kIntAssignable<SeqNanos> && !kImplicitFromInt<SeqNanos> && !kImplicitToInt<SeqNanos>,
              "SeqNanos holds the same no-implicit-integer line as the other two");

static_assert(!kCrossAssignable<UnixNanos, MonoNanos>,
              "a monotonic reading must not assign into a wall-clock field");
static_assert(!kCrossAssignable<MonoNanos, UnixNanos>,
              "a wall-clock reading must not assign into a monotonic field");
static_assert(!kCrossSubtractable<UnixNanos, MonoNanos>,
              "cross-domain subtraction is the bug the types exist to stop");
static_assert(!kCrossSubtractable<MonoNanos, UnixNanos>,
              "cross-domain subtraction is the bug the types exist to stop");
static_assert(!kIntAssignable<UnixNanos> && !kIntAssignable<MonoNanos>,
              "raw integers enter only through fromRaw, where the domain is named");
static_assert(!kImplicitFromInt<UnixNanos> && !kImplicitFromInt<MonoNanos>,
              "an implicit path from int64 reopens the cross-domain route");
static_assert(!kImplicitToInt<UnixNanos> && !kImplicitToInt<MonoNanos>,
              "an implicit path to int64 reopens the cross-domain route");

// Layout is part of the contract: these fields sit inside memcpy'd structs and
// wire records, and the wrapper must be invisible there.
static_assert(sizeof(UnixNanos) == 8 && alignof(UnixNanos) == 8);
static_assert(sizeof(MonoNanos) == 8 && alignof(MonoNanos) == 8);
static_assert(std::is_trivially_copyable_v<UnixNanos> &&
              std::is_trivially_copyable_v<MonoNanos>);

TEST(TimeDomains, SameDomainArithmetic)
{
  const UnixNanos a = UnixNanos::fromRaw(1'000);
  const UnixNanos b = UnixNanos::fromRaw(250);
  const DurationNs d = a - b;
  EXPECT_EQ(d.count(), 750);
  EXPECT_EQ((b + d).raw(), 1'000);
  EXPECT_EQ((a - d).raw(), 250);

  const MonoNanos m = MonoNanos::fromRaw(500);
  EXPECT_EQ((m + DurationNs{100}).raw(), 600u);
  EXPECT_EQ((MonoNanos::fromRaw(600) - m).count(), 100);
}

TEST(TimeDomains, ZeroMeansUnset)
{
  UnixNanos u{};
  MonoNanos m{};
  EXPECT_FALSE(static_cast<bool>(u));
  EXPECT_FALSE(static_cast<bool>(m));
  EXPECT_TRUE(static_cast<bool>(UnixNanos::fromRaw(1)));
}

TEST(TimeDomains, WireLayoutIsTheRawInteger)
{
  // A tape record written before the strong types must read back identically:
  // the bytes of the wrapper are the bytes of the integer.
  const int64_t raw = 1'700'000'000'000'000'000LL;
  UnixNanos u = UnixNanos::fromRaw(raw);
  int64_t bytes = 0;
  std::memcpy(&bytes, &u, sizeof(bytes));
  EXPECT_EQ(bytes, raw);
}
