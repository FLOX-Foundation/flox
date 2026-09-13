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

// fromUnixMs()/fromUnixNs() convert a wall-clock (unix) reading into a
// FloxClock (steady_clock) TimePoint via unix_to_flox_offset_ns(), a
// process-global atomic that init_timebase_mapping() sets exactly once by
// comparing FloxClock::now() against system_clock::now() at the moment it
// runs. Nothing in the live C++ engine startup path calls it -- only a
// Python aggregator binding does -- so the offset defaults to zero. At
// zero, fromUnixMs/fromUnixNs do not merely produce a slightly-off answer:
// they degrade to exactly the bug the strong types above exist to prevent,
// a wall-clock reading reinterpreted as a steady-clock one, silently,
// through an API whose name promises the opposite. This was found while
// fixing a Bybit connector that stored an option's unix-epoch expiry
// straight into a steady-clock field; the conversion helper meant to fix
// that turned out to need this same initialization first, and nothing
// else in the engine calls it either. Grep for "fromUnixNs(trade" in
// include/flox/aggregator/ for other call sites this affects -- bar
// policies convert a trade's exchangeTsNs the same way.
//
// Both tests save and restore the process-global offset so they do not
// leak state into whichever other TimeDomains test happens to run after.
TEST(TimeDomains, ZeroOffsetReproducesTheUnixAsSteadyBug)
{
  const int64_t savedOffset = unix_to_flox_offset_ns().load();
  unix_to_flox_offset_ns().store(0);

  const int64_t nowUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  const TimePoint mapped = fromUnixMs(nowUnixMs);
  const TimePoint steadyNow = now();

  const auto diffDays =
      std::chrono::duration_cast<std::chrono::hours>(mapped - steadyNow).count() / 24;

  // "Now" mapped through a zero offset lands wherever raw unix-epoch
  // nanoseconds happen to sit relative to this machine's steady-clock
  // epoch -- decades away on any real system. The magnitude, not the
  // sign, is the point: this must not be a plausible same-day answer.
  EXPECT_GT(std::llabs(diffDays), 3650)
      << "fromUnixMs(now) landed within 10 years of FloxClock::now() with a "
         "zero offset -- either this test's assumption about the platform's "
         "steady_clock epoch is wrong, or the no-op-without-init bug this "
         "pins has been fixed some other way and this test should be "
         "revisited";

  unix_to_flox_offset_ns().store(savedOffset);
}

TEST(TimeDomains, InitTimebaseMappingFixesTheConversion)
{
  const int64_t savedOffset = unix_to_flox_offset_ns().load();

  init_timebase_mapping();

  const int64_t nowUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  const TimePoint mapped = fromUnixMs(nowUnixMs);
  const TimePoint steadyNow = now();

  const auto diffMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(mapped - steadyNow).count();

  // Once the offset is anchored, "now" mapped through it must land within a
  // second of FloxClock::now() -- generous enough for CI jitter between the
  // two now() calls, tight enough that a regression back to the zero-offset
  // behavior (decades off) cannot pass.
  EXPECT_LT(std::llabs(diffMs), 1000);

  unix_to_flox_offset_ns().store(savedOffset);
}
