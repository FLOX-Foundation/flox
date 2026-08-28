/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/util/system_clock.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
// Bounds for a plausible wall-clock reading: after 2020-01-01 and before 2100.
// This distinguishes a real live clock from a simulated/zero value.
constexpr UnixNanos kAfter2020 = UnixNanos{1'577'836'800'000'000'000LL};
constexpr UnixNanos kBefore2100 = UnixNanos{4'102'444'800'000'000'000LL};
}  // namespace

TEST(SystemClock, NowNsReturnsWallClockUnixNs)
{
  SystemClock c;
  const UnixNanos t = c.nowNs();
  EXPECT_GT(t, kAfter2020);
  EXPECT_LT(t, kBefore2100);
}

TEST(SystemClock, AdvanceToIsNoOpDoesNotRewind)
{
  SystemClock c;
  c.advanceTo(UnixNanos{});  // real time is not advanceable/rewindable
  EXPECT_GT(c.nowNs(), kAfter2020);
}

TEST(SystemClock, UsableThroughIClockInterface)
{
  SystemClock c;
  IClock& clk = c;  // drop-in wherever an IClock is expected (parity with SimulatedClock)
  EXPECT_GT(clk.nowNs(), kAfter2020);
}
