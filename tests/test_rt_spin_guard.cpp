/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include "flox/util/performance/rt_spin_guard.h"

using flox::BackoffMode;
using flox::performance::resolveRtSpinGuard;
using flox::performance::RtSpinPolicy;

TEST(RtSpinGuard, NoRtRequestedPassesThrough)
{
  for (auto mode : {BackoffMode::AGGRESSIVE, BackoffMode::ADAPTIVE, BackoffMode::RELAXED})
  {
    const auto d = resolveRtSpinGuard(false, false, mode);
    EXPECT_FALSE(d.guardTriggered);
    EXPECT_FALSE(d.applyRtPriority);
    EXPECT_EQ(d.backoffMode, mode);
  }
}

TEST(RtSpinGuard, RtOnIsolatedCoreAllowsAggressive)
{
  const auto d = resolveRtSpinGuard(true, true, BackoffMode::AGGRESSIVE);
  EXPECT_FALSE(d.guardTriggered);
  EXPECT_TRUE(d.applyRtPriority);
  EXPECT_EQ(d.backoffMode, BackoffMode::AGGRESSIVE);
}

TEST(RtSpinGuard, RtOnSharedCoreWithSleepingBackoffIsFine)
{
  for (auto mode : {BackoffMode::ADAPTIVE, BackoffMode::RELAXED})
  {
    const auto d = resolveRtSpinGuard(true, false, mode);
    EXPECT_FALSE(d.guardTriggered);
    EXPECT_TRUE(d.applyRtPriority);
    EXPECT_EQ(d.backoffMode, mode);
  }
}

TEST(RtSpinGuard, HazardDowngradesBackoffByDefault)
{
  const auto d = resolveRtSpinGuard(true, false, BackoffMode::AGGRESSIVE);
  EXPECT_TRUE(d.guardTriggered);
  EXPECT_TRUE(d.applyRtPriority);
  EXPECT_EQ(d.backoffMode, BackoffMode::ADAPTIVE);
}

TEST(RtSpinGuard, HazardWithRefusePolicyDropsRtKeepsBackoff)
{
  const auto d =
      resolveRtSpinGuard(true, false, BackoffMode::AGGRESSIVE, RtSpinPolicy::REFUSE);
  EXPECT_TRUE(d.guardTriggered);
  EXPECT_FALSE(d.applyRtPriority);
  EXPECT_EQ(d.backoffMode, BackoffMode::AGGRESSIVE);
}
