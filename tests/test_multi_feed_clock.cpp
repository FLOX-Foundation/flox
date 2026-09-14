/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/feed/multi_feed_clock.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

constexpr SymbolId BTC = 1;
constexpr SymbolId ETH = 2;
constexpr int64_t kSecondNs = 1'000'000'000;

}  // namespace

// ---------------------------------------------------------------------------
// The WaitForAll timeout fallback must work from the very first
// tick, not only after the clock has already fired once.
// ---------------------------------------------------------------------------

TEST(MultiFeedClockTest, TimeoutFallbackFiresBeforeFirstFullFire)
{
  // ETH never ticks at all. Docs promise: "if the timeout elapses first,
  // fire anyway". Previously this never happened before the first full
  // WaitForAll fire, because the fallback condition was `_lastFireTs > 0`,
  // and `_lastFireTs` starts at (and resets to) 0.
  MultiFeedClock clock({BTC, ETH}, FeedClockPolicy::WaitForAll, /*timeoutMs=*/200, /*leader=*/0,
                       /*stalenessBudgetMs=*/0);

  int fired = 0;
  for (int i = 0; i < 1000; ++i)
  {
    auto snap = clock.tick(i * (kSecondNs / 10), BTC);  // one BTC tick every 100ms, 100s total
    if (snap.fired)
    {
      ++fired;
    }
  }

  EXPECT_GT(fired, 0) << "the 200ms timeout must fall back to firing even though "
                         "WaitForAll's condition (both feeds seen) is never met";
}

TEST(MultiFeedClockTest, TimeoutFallbackStillWorksAfterAFullFire)
{
  MultiFeedClock clock({BTC, ETH}, FeedClockPolicy::WaitForAll, /*timeoutMs=*/200, /*leader=*/0,
                       /*stalenessBudgetMs=*/0);

  // Establish one full WaitForAll fire.
  EXPECT_FALSE(clock.tick(0, BTC).fired);
  EXPECT_TRUE(clock.tick(100'000'000, ETH).fired);

  // ETH goes silent; BTC keeps ticking every 300ms (> 200ms timeout).
  int fired = 0;
  for (int i = 1; i <= 10; ++i)
  {
    if (clock.tick(100'000'000 + i * 300'000'000LL, BTC).fired)
    {
      ++fired;
    }
  }

  EXPECT_EQ(fired, 10);
}

TEST(MultiFeedClockTest, TimeoutFallbackWorksWhenFirstFullFireIsAtTimestampZero)
{
  // A related edge case: if the first full fire lands
  // exactly on tsNs == 0 (a backtest replayed from a zero-based clock),
  // the old `_lastFireTs > 0` check made the timeout permanently
  // indistinguishable from "never fired".
  MultiFeedClock clock({BTC, ETH}, FeedClockPolicy::WaitForAll, /*timeoutMs=*/200, /*leader=*/0,
                       /*stalenessBudgetMs=*/0);

  // Both feeds tick at ts=0: WaitForAll's condition is met on the second
  // tick, which also happens to be at ts=0.
  EXPECT_FALSE(clock.tick(0, BTC).fired);
  EXPECT_TRUE(clock.tick(0, ETH).fired);

  // ETH goes silent from here on; BTC ticks every 300ms.
  int fired = 0;
  for (int i = 1; i <= 10; ++i)
  {
    if (clock.tick(i * 300'000'000LL, BTC).fired)
    {
      ++fired;
    }
  }

  EXPECT_EQ(fired, 10) << "a first fire at ts=0 must not disable the timeout forever";
}

// ---------------------------------------------------------------------------
// Existing (previously untested in C++) policy behavior, ported from
// python/tests/test_feed_clock.py so the C++ primitive has direct coverage
// too.
// ---------------------------------------------------------------------------

TEST(MultiFeedClockTest, WaitForAllFiresAfterBothFeeds)
{
  MultiFeedClock clock({BTC, ETH}, FeedClockPolicy::WaitForAll, /*timeoutMs=*/200, /*leader=*/0,
                       /*stalenessBudgetMs=*/0);

  auto r1 = clock.tick(kSecondNs, BTC);
  EXPECT_FALSE(r1.fired);

  auto r2 = clock.tick(kSecondNs + 100'000'000, ETH);
  EXPECT_TRUE(r2.fired);
  EXPECT_EQ(r2.triggeredBy, ETH);

  // Accumulator resets after a fire.
  auto r3 = clock.tick(kSecondNs + 200'000'000, BTC);
  EXPECT_FALSE(r3.fired);
}

TEST(MultiFeedClockTest, FireOnAnyFiresEveryTick)
{
  MultiFeedClock clock({BTC, ETH}, FeedClockPolicy::FireOnAny, /*timeoutMs=*/0, /*leader=*/0,
                       /*stalenessBudgetMs=*/0);

  EXPECT_TRUE(clock.tick(kSecondNs, BTC).fired);
  EXPECT_TRUE(clock.tick(kSecondNs + 1, ETH).fired);
}

TEST(MultiFeedClockTest, LeaderFollowerRequiresFreshFollower)
{
  MultiFeedClock clock({BTC, ETH}, FeedClockPolicy::LeaderFollower, /*timeoutMs=*/0, /*leader=*/BTC,
                       /*stalenessBudgetMs=*/200);

  EXPECT_FALSE(clock.tick(kSecondNs, ETH).fired);
  EXPECT_TRUE(clock.tick(kSecondNs + 50'000'000, BTC).fired);
  // Follower now stale (>200ms).
  EXPECT_FALSE(clock.tick(kSecondNs + 500'000'000, BTC).fired);
}

// ---------------------------------------------------------------------------
// docs/how-to/multi-feed-clock.md claimed an out-of-band symbol's
// tick "updates the per-symbol last-seen timestamp". It does not -- the
// early return happens before any state is touched. This pins the actual
// (and now correctly documented) behavior: an out-of-band tick never fires
// and never affects the snapshot for registered symbols.
// ---------------------------------------------------------------------------

TEST(MultiFeedClockTest, OutOfBandSymbolNeverFiresAndDoesNotAffectRegisteredState)
{
  constexpr SymbolId UNREGISTERED = 99;
  MultiFeedClock clock({BTC, ETH}, FeedClockPolicy::WaitForAll, /*timeoutMs=*/200, /*leader=*/0,
                       /*stalenessBudgetMs=*/0);

  auto before = clock.tick(kSecondNs, BTC);
  EXPECT_FALSE(before.fired);

  auto oob = clock.tick(kSecondNs + 1, UNREGISTERED);
  EXPECT_FALSE(oob.fired);
  EXPECT_EQ(oob.triggeredBy, UNREGISTERED);

  // BTC's last-seen timestamp must be exactly what it was before the
  // out-of-band tick -- an unregistered symbol has no slot to update.
  auto afterBtc = clock.tick(kSecondNs + 2, BTC);
  EXPECT_EQ(afterBtc.lastTsNs[0], kSecondNs + 2);
}
